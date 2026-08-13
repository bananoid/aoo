#include "udp_server.hpp"

#include "common/log.hpp"
#include "common/utils.hpp"

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

namespace aoo {

namespace {

void accumulate_maximum(std::atomic<uint64_t>& target, uint64_t value) {
    auto maximum = target.load(std::memory_order_relaxed);
    while (value > maximum
           && !target.compare_exchange_weak(
               maximum,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed
           )) {}
}

double monotonic_ticks_to_seconds(uint64_t ticks) {
#if defined(__APPLE__)
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    return static_cast<double>(ticks)
        * static_cast<double>(timebase.numer)
        / static_cast<double>(timebase.denom)
        * 1.0e-9;
#else
    (void)ticks;
    return 0;
#endif
}

} // namespace

void udp_server::start(int port, receive_handler receive, bool threaded) {
    do_close();

    receive_handler_ = std::move(receive);

    // Don't try to reuse ports because it would lead to silent errors
    // if the port is already taken by another application.
    // Also, it can cause deadlocks when trying to signal the socket
    // and join the network thread.
    // TODO: figure out if some operating systems let UDP sockets linger.
    try {
        socket_ = udp_socket(port_tag{}, port, false);
        bind_addr_ = socket_.address();
    } catch (const socket_error& e) {
        throw udp_error(e);
    }

    reset_receive_timing_statistics();
    socket_.enable_monotonic_receive_timestamps(true);

    if (send_buffer_size_ > 0) {
        try {
            socket_.set_send_buffer_size(send_buffer_size_);
        } catch (const socket_error& e) {
            socket::print_error(e.code(),
                "udp_server: could not send send buffer size");
        }
    }

    if (receive_buffer_size_ > 0) {
        try {
            socket_.set_receive_buffer_size(receive_buffer_size_);
        } catch (const socket_error& e) {
            socket::print_error(e.code(),
                "udp_server: could not send receive buffer size");
        }
    }

    running_.store(true);
    threaded_ = threaded;
    if (threaded_) {
        packet_queue_.clear();
        // TODO: lower thread priority?
        thread_ = std::thread([this](){
            try {
                this->receive(-1.0);
            } catch (const udp_error& e) {
                LOG_DEBUG("udp_server: thread function failed: " << e.what());
                // TODO: report error to main thread
            }
            running_.store(false);
        });
    }
}

bool udp_server::run(double timeout) {
    if (timeout >= 0) {
        // 1) with timeout
        if (threaded_) {
            // a) threaded
            if (timeout == 0) {
                if (!packet_queue_.empty()) {
                    packet_queue_.consume_all([this](auto& packet){
                        receive_handler_(packet.data.data(), packet.data.size(), packet.address);
                    });
                    return true;
                } else {
                    return false;
                }
            } else {
                if (event_.wait_for(timeout)) {
                    packet_queue_.consume_all([this](auto& packet){
                        receive_handler_(packet.data.data(), packet.data.size(), packet.address);
                    });
                    return true;
                } else {
                    return false;
                }
            }
        } else {
            // b) non-threaded
#if 1
            if (receive(timeout)) {
                // drain sockets without blocking
                while (receive(0)) {}
                return true;
            } else {
                return false;
            }
#else
            // only a single packet at the time
            return receive(timeout);
#endif
        }
    } else {
        // 2) blocking
        if (threaded_) {
            // a) threaded
            while (running_.load()) {
                packet_queue_.consume_all([&](auto& packet){
                    receive_handler_(packet.data.data(), packet.data.size(), packet.address);
                });
                // wait for packets
                event_.wait();
            }
        } else {
            // b) non-threaded
            while (running_.load()) {
                receive(-1.0);
            }
        }

        do_close();

        return true;
    }
}

void udp_server::stop() {
    bool running = running_.exchange(false);
    if (running) {
        // wake up receive
        if (!socket_.signal()) {
            // force wakeup by closing the socket.
            // this is not nice and probably undefined behavior,
            // the MSDN docs explicitly forbid it!
            socket_.close();
        }
        if (threaded_) {
            // wake up main thread
            event_.set();
            // join receive thread
            if (thread_.joinable()) {
                thread_.join();
            }
        }
    }
}

void udp_server::notify() {
    if (threaded_) {
        event_.set(); // wake up main thread
    } else {
        socket_.signal();
    }
}

void udp_server::do_close() {
    socket_.close();
    bind_addr_.clear();
    if (thread_.joinable()) {
        thread_.join();
    }
}

udp_server::~udp_server() {
    stop();
    do_close();
}

void udp_server::reset_receive_timing_statistics() {
    receive_datagram_count_.store(0, std::memory_order_relaxed);
    kernel_timestamp_count_.store(0, std::memory_order_relaxed);
    last_kernel_timestamp_.store(0, std::memory_order_relaxed);
    latest_kernel_datagram_gap_.store(0, std::memory_order_relaxed);
    maximum_kernel_datagram_gap_.store(0, std::memory_order_relaxed);
    latest_kernel_to_receive_delay_.store(0, std::memory_order_relaxed);
    maximum_kernel_to_receive_delay_.store(0, std::memory_order_relaxed);
}

void udp_server::observe_receive_timing(uint64_t kernel_timestamp) {
    receive_datagram_count_.fetch_add(1, std::memory_order_relaxed);
#if defined(__APPLE__)
    if (kernel_timestamp == 0) {
        return;
    }
    kernel_timestamp_count_.fetch_add(1, std::memory_order_relaxed);
    const uint64_t previous = last_kernel_timestamp_.exchange(
        kernel_timestamp,
        std::memory_order_relaxed
    );
    if (previous > 0 && kernel_timestamp >= previous) {
        const uint64_t gap = kernel_timestamp - previous;
        latest_kernel_datagram_gap_.store(gap, std::memory_order_relaxed);
        accumulate_maximum(maximum_kernel_datagram_gap_, gap);
    }
    const uint64_t received = mach_absolute_time();
    if (received >= kernel_timestamp) {
        const uint64_t delay = received - kernel_timestamp;
        latest_kernel_to_receive_delay_.store(delay, std::memory_order_relaxed);
        accumulate_maximum(maximum_kernel_to_receive_delay_, delay);
    }
#else
    (void)kernel_timestamp;
#endif
}

void udp_server::get_receive_timing_statistics(
        udp_receive_timing_statistics& statistics) const {
    statistics.datagram_count = receive_datagram_count_.load(
        std::memory_order_relaxed
    );
    statistics.kernel_timestamp_count = kernel_timestamp_count_.load(
        std::memory_order_relaxed
    );
    statistics.latest_kernel_datagram_gap = monotonic_ticks_to_seconds(
        latest_kernel_datagram_gap_.load(std::memory_order_relaxed)
    );
    statistics.maximum_kernel_datagram_gap = monotonic_ticks_to_seconds(
        maximum_kernel_datagram_gap_.load(std::memory_order_relaxed)
    );
    statistics.latest_kernel_to_receive_delay = monotonic_ticks_to_seconds(
        latest_kernel_to_receive_delay_.load(std::memory_order_relaxed)
    );
    statistics.maximum_kernel_to_receive_delay = monotonic_ticks_to_seconds(
        maximum_kernel_to_receive_delay_.load(std::memory_order_relaxed)
    );
}

bool udp_server::receive(double timeout) {
    try {
        aoo::ip_address address;
        uint64_t kernelTimestamp = 0;
        auto [success, result] = socket_.receive(buffer_.data(), buffer_.size(),
                                                 address, timeout,
                                                 &kernelTimestamp);
        if (success) {
            if (result > 0) {
                observe_receive_timing(kernelTimestamp);
                if (threaded_) {
                    packet_queue_.produce([&, len=result](auto& packet){
                        packet.data.assign(buffer_.data(), buffer_.data() + len);
                        packet.address = address;
                    });
                    event_.set(); // notify main thread (if blocking)
                } else {
                    receive_handler_(buffer_.data(), result, address);
                }
            }
            // ignore timeout or empty packet (used for signalling)
            return true;
        } else {
            // timeout
            return false;
        }
    } catch (const socket_error& e) {
#ifdef _WIN32
        // ignore ICMP Port Unreachable message!
        if (e.code() == WSAECONNRESET) {
            return true; // continue
        }
#else
        if (e.code() == EINTR){
            return true; // continue
        }
#endif
        // notify main thread (if blocking)
        if (threaded_) {
            running_.store(false);
            event_.set();
        }

        throw udp_error(e);
    }
}

} // aoo
