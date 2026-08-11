#include "AOOAppleBridge.h"

#include "aoo.h"
#include "aoo_client.h"
#include "aoo_events.h"
#include "aoo_low_latency.h"
#include "aoo_sink.h"
#include "aoo_source.h"
#include "codec/aoo_pcm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

namespace {

constexpr int32_t kMaximumChannelCount = 64;
constexpr int32_t kDefaultMaximumBlockSize = 4096;
constexpr int32_t kMaximumPacketSize = AOO_MAX_PACKET_SIZE;
constexpr uint64_t kMinimumSenderProcessQueueCapacity = 8;
constexpr uint64_t kSenderGapQueueCapacity = 256;
constexpr double kMinimumReceiverBufferCapacitySeconds = 0.1;
constexpr double kStreamTimeSendIntervalSeconds = 0.25;
constexpr double kPingIntervalSeconds = 0.25;
constexpr AooDataType kSourceTimingTimestampMessageType =
    static_cast<AooDataType>(kAooDataUser + 1);
constexpr int32_t kEncodedNtpTimestampByteCount = 8;
constexpr int32_t kSourceTimingTimestampByteCount =
    kEncodedNtpTimestampByteCount * 3;

std::once_flag gInitializationOnce;
bool gInitializationSucceeded = false;

bool retainAOO() {
    std::call_once(gInitializationOnce, [] {
        gInitializationSucceeded = aoo_initialize(nullptr) == kAooOk;
        if (gInitializationSucceeded) {
            std::atexit([] { aoo_terminate(); });
        }
    });
    return gInitializationSucceeded;
}

// AOO 2.0.0's initializer is process-once: aoo_terminate() unloads codecs but
// its internal initialization guard is not reset. Keep the library alive until
// the registered process-exit handler so sender/receiver instances can restart.
void releaseAOO() {}

void configureNetworkThread() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
}

int32_t clampedPort(int32_t port, int32_t fallback) {
    return port > 0 && port <= UINT16_MAX ? port : fallback;
}

int32_t clampedSenderPort(int32_t port) {
    return port == 0 ? 0 : clampedPort(port, 9998);
}

int32_t clampedChannelCount(int32_t channelCount) {
    return std::clamp(channelCount, 1, kMaximumChannelCount);
}

int32_t clampedBlockSize(int32_t blockSize) {
    return std::clamp(blockSize, 1, kDefaultMaximumBlockSize);
}

int32_t clampedPacketSize(int32_t packetSize) {
    return std::clamp(packetSize, 256, kMaximumPacketSize);
}

double clampedSampleRate(double sampleRate) {
    return std::isfinite(sampleRate) && sampleRate >= 8000.0 && sampleRate <= 384000.0
        ? sampleRate
        : 48000.0;
}

double clampedLatencySeconds(double milliseconds) {
    if (!std::isfinite(milliseconds)) {
        milliseconds = 12.0;
    }
    return std::clamp(milliseconds, 2.0, 200.0) * 0.001;
}

double receiverBufferCapacitySeconds(
    double targetLatencySeconds,
    int32_t transportProfile,
    int32_t blockFrames,
    double sampleRate
) {
    const double blockSeconds = static_cast<double>(blockFrames) / sampleRate;
    if (transportProfile == AOOAppleTransportProfileDeterministicWired) {
        return std::max(targetLatencySeconds, blockSeconds * 8.0);
    }
    return std::max(0.2, targetLatencySeconds);
}

int64_t monotonicNanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

void accumulatePeak(std::atomic<float>& destination, float peak) {
    float current = destination.load(std::memory_order_relaxed);
    while (peak > current && !destination.compare_exchange_weak(
        current,
        peak,
        std::memory_order_relaxed,
        std::memory_order_relaxed
    )) {}
}

void accumulateMaximum(std::atomic<double>& destination, double value) {
    double current = destination.load(std::memory_order_relaxed);
    while (value > current && !destination.compare_exchange_weak(
        current,
        value,
        std::memory_order_relaxed,
        std::memory_order_relaxed
    )) {}
}

inline AooSample finiteSample(AooSample sample) {
    return std::isfinite(sample) ? sample : 0;
}

void encodeUInt64(uint64_t value, AooByte *bytes) {
    for (int32_t index = kEncodedNtpTimestampByteCount - 1; index >= 0; --index) {
        bytes[index] = static_cast<AooByte>(value & 0xff);
        value >>= 8;
    }
}

uint64_t decodeUInt64(const AooByte *bytes) {
    uint64_t value = 0;
    for (int32_t index = 0; index < kEncodedNtpTimestampByteCount; ++index) {
        value = (value << 8) | bytes[index];
    }
    return value;
}

struct SenderMetrics {
    std::atomic<int32_t> ready{0};
    std::atomic<int32_t> enabled{0};
    std::atomic<int32_t> hasDestination{0};
    std::atomic<int32_t> peerResponsive{0};
    std::atomic<int64_t> lastPeerPingNanoseconds{0};
    std::atomic<int32_t> lastError{kAooOk};
    std::atomic<uint64_t> processCalls{0};
    std::atomic<uint64_t> processedFrames{0};
    std::atomic<uint64_t> processErrors{0};
    std::atomic<uint64_t> handoffDrops{0};
    std::atomic<uint64_t> resentFrames{0};
    std::atomic<uint64_t> pingEvents{0};
    std::atomic<double> handoffLatencyMilliseconds{0};
    std::atomic<double> maximumHandoffLatencyMilliseconds{0};
    std::atomic<double> sourcePresentationLeadMilliseconds{0};
    std::atomic<double> roundTripMilliseconds{0};
    std::atomic<double> packetLoss{0};
};

struct SenderProcessBlock {
    std::vector<AooSample> storage;
    std::array<AooSample *, kMaximumChannelCount> channelPointers{};
    int32_t frameCount = 0;
    bool isSilence = false;
    uint64_t sourceSamplePosition = 0;
    AooNtpTime sourceNtpTime = 0;
    AooNtpTime sourcePresentationNtpTime = 0;
    AooNtpTime enqueueNtpTime = 0;
    uint64_t generation = 0;
};

struct SenderProcessGap {
    uint64_t sourceSamplePosition = 0;
    AooNtpTime sourceNtpTime = 0;
    AooNtpTime sourcePresentationNtpTime = 0;
    AooNtpTime enqueueNtpTime = 0;
    uint64_t generation = 0;
};

struct ReceiverMetrics {
    std::atomic<int32_t> ready{0};
    std::atomic<int32_t> streamActive{0};
    std::atomic<int32_t> streamState{kAooStreamStateInactive};
    std::atomic<int32_t> lastError{kAooOk};
    std::atomic<int32_t> sourceChannelCount{0};
    std::atomic<int32_t> sourceSampleRate{0};
    std::atomic<int32_t> sourceBlockSize{0};
    std::atomic<uint64_t> processCalls{0};
    std::atomic<uint64_t> processedFrames{0};
    std::atomic<uint64_t> processErrors{0};
    std::atomic<uint64_t> processBlockMismatches{0};
    std::atomic<uint64_t> streamStarts{0};
    std::atomic<uint64_t> streamActiveTransitions{0};
    std::atomic<uint64_t> streamBufferingTransitions{0};
    std::atomic<uint64_t> streamInactiveTransitions{0};
    std::atomic<int32_t> lastProcessFrameCount{0};
    std::atomic<int32_t> minimumProcessFrameCount{INT32_MAX};
    std::atomic<int32_t> maximumProcessFrameCount{0};
    std::atomic<uint64_t> bufferUnderruns{0};
    std::atomic<uint64_t> bufferOverruns{0};
    std::atomic<uint64_t> droppedBlocks{0};
    std::atomic<uint64_t> resentBlocks{0};
    std::atomic<uint64_t> sourceXRuns{0};
    std::atomic<uint64_t> concealments{0};
    std::atomic<uint64_t> reacquisitions{0};
    std::atomic<uint64_t> incompatibleStreams{0};
    std::atomic<uint64_t> adaptiveAdjustments{0};
    std::atomic<uint64_t> pingEvents{0};
    std::atomic<uint64_t> aooStreamTimeEvents{0};
    std::atomic<uint64_t> streamTimeEvents{0};
    std::atomic<uint64_t> presentationTimestampEvents{0};
    std::atomic<uint64_t> sourceSamplePosition{0};
    std::atomic<double> sourceLatencyMilliseconds{0};
    std::atomic<double> sinkLatencyMilliseconds{0};
    std::atomic<double> jitterBufferLatencyMilliseconds{0};
    std::atomic<double> rawRoundTripMilliseconds{0};
    std::atomic<double> rawClockOffsetMilliseconds{0};
    std::atomic<double> rawAOOStreamTimeDeltaMilliseconds{0};
    std::atomic<double> rawStreamTimeDeltaMilliseconds{0};
    std::atomic<double> rawPresentationToSinkCallbackDeltaMilliseconds{0};
    std::atomic<double> outputPresentationLatencyMilliseconds{0};
};

} // namespace

struct AOOAppleSender {
    AooSource *source = nullptr;
    AooClient *client = nullptr;
    int32_t localPort = 0;
    int32_t channelCount = 0;
    int32_t sampleRate = 0;
    int32_t maximumBlockSize = 0;
    int32_t streamBlockSize = 0;
    int32_t packetSize = 0;
    int32_t transportProfile = AOOAppleTransportProfileDeterministicWired;
    int32_t pcmFormat = AOOApplePCMFormatFloat32;
    int32_t requiredSourceChannelCount = 0;
    AooSocketFlags socketType = kAooSocketDefault;
    std::array<int32_t, kMaximumChannelCount> sourceChannelIndices{};
    std::array<AooByte, AOO_LOW_LATENCY_STREAM_CONFIGURATION_SIZE>
        streamMetadata{};
    int32_t streamMetadataSize = 0;
    std::vector<AooSample> silenceStorage;
    std::array<AooSample *, kMaximumChannelCount> silencePointers{};
    std::vector<AooSample> pendingStorage;
    std::array<AooSample *, kMaximumChannelCount> pendingChannelPointers{};
    int32_t pendingFrameCount = 0;
    uint64_t pendingGeneration = 0;
    bool pendingIsSilence = true;
    uint64_t pendingSourceSamplePosition = 0;
    AooNtpTime pendingSourceNtpTime = 0;
    AooNtpTime pendingSourcePresentationNtpTime = 0;
    std::vector<SenderProcessBlock> processQueue;
    std::atomic<uint64_t> processWriteIndex{0};
    std::atomic<uint64_t> processReadIndex{0};
    std::array<SenderProcessGap, kSenderGapQueueCapacity> processGapQueue{};
    std::atomic<uint64_t> processGapWriteIndex{0};
    std::atomic<uint64_t> processGapReadIndex{0};
    std::atomic<bool> processThreadShouldRun{false};
    std::atomic<uint64_t> streamGeneration{0};
    std::atomic<AooNtpTime> lastPresentationMessageNtpTime{0};
    std::thread processThread;
    std::thread receiveThread;
    std::mutex destinationMutex;
    SenderMetrics metrics;

    ~AOOAppleSender() {
        metrics.enabled.store(0, std::memory_order_release);
        processThreadShouldRun.store(false, std::memory_order_release);
        if (processThread.joinable()) {
            processThread.join();
        }
        if (source) {
            AooSource_stopStream(source, 0);
        }
        if (client) {
            AooClient_stop(client);
        }
        if (receiveThread.joinable()) {
            receiveThread.join();
        }
        if (client && source) {
            AooClient_removeSource(client, source);
        }
        if (source) {
            AooSource_free(source);
        }
        if (client) {
            AooClient_free(client);
        }
        releaseAOO();
    }
};

struct AOOAppleReceiver {
    AooSink *sink = nullptr;
    AooClient *client = nullptr;
    int32_t localPort = 0;
    int32_t channelCount = 0;
    int32_t sampleRate = 0;
    int32_t maximumBlockSize = 0;
    int32_t streamBlockSize = 0;
    bool fixedBlockSizeEnabled = false;
    int32_t packetSize = 0;
    int32_t transportProfile = AOOAppleTransportProfileDeterministicWired;
    int32_t pcmFormat = AOOApplePCMFormatFloat32;
    std::atomic<int32_t> activeTransportProfile{
        AOOAppleTransportProfileDeterministicWired
    };
    std::atomic<int32_t> activePcmFormat{AOOApplePCMFormatFloat32};
    AooLowLatencyStreamConfiguration expectedStreamConfiguration{};
    std::vector<AooSample> sourceChannelStorage;
    std::vector<AooSample> channelStorage;
    std::array<AooSample *, kMaximumChannelCount> sourceChannelPointers{};
    std::array<AooSample *, kMaximumChannelCount> channelPointers{};
    std::array<std::atomic<int32_t>, kMaximumChannelCount> sourceChannelMap;
    std::atomic<int32_t> sourceChannelMapCount{0};
    std::atomic<uint64_t> sourceChannelMapRevision{0};
    std::mutex sourceChannelMapMutex;
    std::unique_ptr<std::atomic<float>[]> channelPeaks;
    std::atomic<int32_t> monitorPairStartChannel{0};
    std::atomic<double> targetLatencySeconds{0};
    AooNtpTime currentSinkProcessNtpTime = 0;
    std::mutex sourceEndpointMutex;
    AooSockAddrStorage sourceEndpointAddress{};
    AooAddrSize sourceEndpointAddressSize = 0;
    AooId sourceEndpointID = kAooIdNone;
    bool hasSourceEndpoint = false;
    std::atomic<uint64_t> sourceEndpointFingerprint{0};
    std::thread sendThread;
    std::thread receiveThread;
    std::atomic<bool> eventThreadShouldRun{false};
    std::thread eventThread;
    std::atomic<bool> adaptiveThreadShouldRun{false};
    std::thread adaptiveThread;
    ReceiverMetrics metrics;

    ~AOOAppleReceiver() {
        eventThreadShouldRun.store(false, std::memory_order_release);
        adaptiveThreadShouldRun.store(false, std::memory_order_release);
        if (eventThread.joinable()) {
            eventThread.join();
        }
        if (adaptiveThread.joinable()) {
            adaptiveThread.join();
        }
        if (client) {
            AooClient_stop(client);
        }
        if (sendThread.joinable()) {
            sendThread.join();
        }
        if (receiveThread.joinable()) {
            receiveThread.join();
        }
        if (client && sink) {
            AooClient_removeSink(client, sink);
        }
        if (sink) {
            AooSink_free(sink);
        }
        if (client) {
            AooClient_free(client);
        }
        releaseAOO();
    }
};

AooError startSenderStream(AOOAppleSender *sender) {
    const AooData metadata = {
        kAooDataBinary,
        sender->streamMetadata.data(),
        static_cast<AooSize>(sender->streamMetadataSize),
    };
    sender->lastPresentationMessageNtpTime.store(0, std::memory_order_release);
    return AooSource_startStream(sender->source, 0, &metadata);
}

bool applyReceiverChannelMap(
    AOOAppleReceiver *receiver,
    const AooData *metadata
) {
    std::array<int32_t, kMaximumChannelCount> channelMap{};
    AooLowLatencyStreamConfiguration negotiatedConfiguration{};
    bool hasNegotiatedConfiguration = false;
    const int32_t channelMapCount = [&] {
        if (!metadata) {
            return int32_t{0};
        }
        if (metadata->type != kAooDataBinary || !metadata->data) {
            return int32_t{-1};
        }
        if (aoo_lowLatencyStreamConfigurationDecode(
                metadata->data,
                metadata->size,
                &negotiatedConfiguration
            ) != kAooOk) {
            return int32_t{-1};
        }
        hasNegotiatedConfiguration = true;
        const auto& configuration = negotiatedConfiguration;
        const auto& expected = receiver->expectedStreamConfiguration;
        const bool acceptsAutomaticProfile = receiver->transportProfile
            == AOOAppleTransportProfileAutomatic;
        AooUInt32 requiredCapabilities =
            kAooLowLatencyCapabilityAbsoluteSamplePosition
            | kAooLowLatencyCapabilityChannelMap
            | kAooLowLatencyCapabilityDynamicResampling;
        if (configuration.profile == kAooLowLatencyProfileAdaptiveWireless) {
            requiredCapabilities |= kAooLowLatencyCapabilityDeadlineResend;
        }
        const int32_t count = configuration.channelMapCount;
        if (count <= 0 || count > receiver->channelCount
            || configuration.protocolVersion != expected.protocolVersion
            || (!acceptsAutomaticProfile
                && configuration.profile != expected.profile)
            || (!acceptsAutomaticProfile
                && configuration.pcmFormat != expected.pcmFormat)
            || configuration.channelCount > expected.channelCount
            || configuration.blockFrames != expected.blockFrames
            || (!acceptsAutomaticProfile
                && configuration.datagramBytes != expected.datagramBytes)
            || (acceptsAutomaticProfile
                && configuration.datagramBytes > expected.datagramBytes)
            || configuration.sampleRate != expected.sampleRate
            || (configuration.capabilities & requiredCapabilities)
                != requiredCapabilities) {
            return int32_t{-1};
        }

        std::array<bool, kMaximumChannelCount> used{};
        for (int32_t channel = 0; channel < count; ++channel) {
            const int32_t mappedChannel = configuration.channelMap[channel];
            if (mappedChannel < 0 || mappedChannel >= receiver->channelCount
                || used[mappedChannel]) {
                return int32_t{-1};
            }
            used[mappedChannel] = true;
            channelMap[channel] = mappedChannel;
        }
        return count;
    }();

    if (channelMapCount < 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(receiver->sourceChannelMapMutex);
    receiver->sourceChannelMapRevision.fetch_add(1, std::memory_order_acq_rel);
    receiver->sourceChannelMapCount.store(0, std::memory_order_relaxed);
    for (int32_t channel = 0; channel < channelMapCount; ++channel) {
        receiver->sourceChannelMap[channel].store(
            channelMap[channel],
            std::memory_order_relaxed
        );
    }
    receiver->sourceChannelMapCount.store(channelMapCount, std::memory_order_relaxed);
    receiver->sourceChannelMapRevision.fetch_add(1, std::memory_order_release);
    if (hasNegotiatedConfiguration) {
        receiver->activeTransportProfile.store(
            negotiatedConfiguration.profile,
            std::memory_order_release
        );
        receiver->activePcmFormat.store(
            negotiatedConfiguration.pcmFormat,
            std::memory_order_release
        );
        receiver->targetLatencySeconds.store(
            static_cast<double>(negotiatedConfiguration.targetLatencyFrames)
                / negotiatedConfiguration.sampleRate,
            std::memory_order_release
        );
        AooSink_setLowLatencyTarget(
            receiver->sink,
            negotiatedConfiguration.targetLatencyFrames
        );
    }
    return true;
}

void setReceiverSourceEndpoint(
    AOOAppleReceiver *receiver,
    const AooEndpoint *endpoint
) {
    std::lock_guard<std::mutex> lock(receiver->sourceEndpointMutex);
    receiver->sourceEndpointFingerprint.store(0, std::memory_order_release);
    receiver->hasSourceEndpoint = false;
    receiver->sourceEndpointAddressSize = 0;
    receiver->sourceEndpointID = kAooIdNone;
    if (!endpoint || !endpoint->address || endpoint->addrlen <= 0
        || endpoint->addrlen > sizeof(receiver->sourceEndpointAddress)) {
        return;
    }
    std::memcpy(
        &receiver->sourceEndpointAddress,
        endpoint->address,
        endpoint->addrlen
    );
    receiver->sourceEndpointAddressSize = endpoint->addrlen;
    receiver->sourceEndpointID = endpoint->id;
    receiver->hasSourceEndpoint = true;
    uint64_t fingerprint = 1469598103934665603ULL;
    const auto *bytes = static_cast<const uint8_t *>(endpoint->address);
    for (AooAddrSize index = 0; index < endpoint->addrlen; ++index) {
        fingerprint = (fingerprint ^ bytes[index]) * 1099511628211ULL;
    }
    fingerprint = (
        fingerprint ^ static_cast<uint32_t>(endpoint->id)
    ) * 1099511628211ULL;
    receiver->sourceEndpointFingerprint.store(
        fingerprint == 0 ? 1 : fingerprint,
        std::memory_order_release
    );
}

uint64_t receiverEndpointFingerprint(const AooEndpoint *endpoint) {
    if (!endpoint || !endpoint->address || endpoint->addrlen <= 0) {
        return 0;
    }
    uint64_t fingerprint = 1469598103934665603ULL;
    const auto *bytes = static_cast<const uint8_t *>(endpoint->address);
    for (AooAddrSize index = 0; index < endpoint->addrlen; ++index) {
        fingerprint = (fingerprint ^ bytes[index]) * 1099511628211ULL;
    }
    fingerprint = (
        fingerprint ^ static_cast<uint32_t>(endpoint->id)
    ) * 1099511628211ULL;
    return fingerprint == 0 ? 1 : fingerprint;
}

bool receiverSourceEndpointMatches(
    AOOAppleReceiver *receiver,
    const AooEndpoint *endpoint
) {
    if (!endpoint || !endpoint->address || endpoint->addrlen <= 0) {
        return false;
    }
    return receiver->sourceEndpointFingerprint.load(std::memory_order_acquire)
        == receiverEndpointFingerprint(endpoint);
}

bool receiverHasSourceEndpoint(AOOAppleReceiver *receiver) {
    return receiver->sourceEndpointFingerprint.load(std::memory_order_acquire) != 0;
}

void applyReceiverSourceFormat(
    AOOAppleReceiver *receiver,
    const AooFormat *format
) {
    if (!format) {
        return;
    }
    receiver->metrics.sourceChannelCount.store(
        format->numChannels,
        std::memory_order_relaxed
    );
    receiver->metrics.sourceSampleRate.store(
        static_cast<int32_t>(std::lround(format->sampleRate)),
        std::memory_order_relaxed
    );
    receiver->metrics.sourceBlockSize.store(
        format->blockSize,
        std::memory_order_relaxed
    );
}

double receiverBufferFillRatio(AOOAppleReceiver *receiver) {
    AooSockAddrStorage address{};
    AooAddrSize addressSize = 0;
    AooId sourceID = kAooIdNone;
    {
        std::lock_guard<std::mutex> lock(receiver->sourceEndpointMutex);
        if (!receiver->hasSourceEndpoint) {
            return -1;
        }
        address = receiver->sourceEndpointAddress;
        addressSize = receiver->sourceEndpointAddressSize;
        sourceID = receiver->sourceEndpointID;
    }
    const AooEndpoint endpoint = {&address, addressSize, sourceID};
    double ratio = -1;
    const AooError result = AooSink_getBufferFillRatio(
        receiver->sink,
        &endpoint,
        &ratio
    );
    return result == kAooOk && std::isfinite(ratio)
        ? std::clamp(ratio, 0.0, 1.0)
        : -1;
}

namespace {

void AOO_CALL handleReceiverStreamMessage(
    void *user,
    const AooStreamMessage *message,
    const AooEndpoint *endpoint
) {
    auto *receiver = static_cast<AOOAppleReceiver *>(user);
    if (!receiver || !message
        || !receiverSourceEndpointMatches(receiver, endpoint)
        || message->type != kSourceTimingTimestampMessageType
        || message->size != kSourceTimingTimestampByteCount
        || !message->data
        || receiver->currentSinkProcessNtpTime == 0) {
        return;
    }

    const uint64_t sourceSamplePosition = decodeUInt64(message->data);
    const AooNtpTime sourceCallbackNtpTime = decodeUInt64(
        message->data + kEncodedNtpTimestampByteCount
    );
    const AooNtpTime sourcePresentationNtpTime = decodeUInt64(
        message->data + kEncodedNtpTimestampByteCount * 2
    );
    receiver->metrics.sourceSamplePosition.store(
        sourceSamplePosition + static_cast<uint64_t>(std::max(0, message->sampleOffset)),
        std::memory_order_relaxed
    );
    if (sourceCallbackNtpTime == 0 || sourcePresentationNtpTime == 0) {
        return;
    }
    const AooNtpTime sinkSampleNtpTime = AOOAppleNTPTimeOffsetFrames(
        receiver->currentSinkProcessNtpTime,
        std::max(0, message->sampleOffset),
        receiver->sampleRate
    );
    const double callbackDeltaMilliseconds = aoo_ntpTimeDuration(
        sourceCallbackNtpTime,
        sinkSampleNtpTime
    ) * 1000.0;
    const double presentationDeltaMilliseconds = aoo_ntpTimeDuration(
        sourcePresentationNtpTime,
        sinkSampleNtpTime
    ) * 1000.0;
    if (!std::isfinite(callbackDeltaMilliseconds)
        || !std::isfinite(presentationDeltaMilliseconds)) {
        return;
    }
    receiver->metrics.rawStreamTimeDeltaMilliseconds.store(
        callbackDeltaMilliseconds,
        std::memory_order_relaxed
    );
    receiver->metrics.rawPresentationToSinkCallbackDeltaMilliseconds.store(
        presentationDeltaMilliseconds,
        std::memory_order_relaxed
    );
    receiver->metrics.streamTimeEvents.fetch_add(1, std::memory_order_release);
    receiver->metrics.presentationTimestampEvents.fetch_add(
        1,
        std::memory_order_release
    );
}

void AOO_CALL handleSenderEvent(
    void *user,
    const AooEvent *event,
    AooThreadLevel
) {
    auto *sender = static_cast<AOOAppleSender *>(user);
    if (!sender || !event) {
        return;
    }
    switch (event->type) {
    case kAooEventInvite:
        AooSource_handleInvite(
            sender->source,
            &event->invite.endpoint,
            event->invite.token,
            kAooTrue
        );
        break;
    case kAooEventUninvite:
        AooSource_handleUninvite(
            sender->source,
            &event->uninvite.endpoint,
            event->uninvite.token,
            kAooTrue
        );
        break;
    case kAooEventSinkPing: {
        sender->metrics.pingEvents.fetch_add(1, std::memory_order_relaxed);
        sender->metrics.peerResponsive.store(1, std::memory_order_release);
        sender->metrics.lastPeerPingNanoseconds.store(
            monotonicNanoseconds(),
            std::memory_order_relaxed
        );
        sender->metrics.packetLoss.store(
            std::clamp(static_cast<double>(event->sinkPing.packetLoss), 0.0, 1.0),
            std::memory_order_relaxed
        );
        const double rtt = aoo_ntpTimeDuration(event->sinkPing.t1, event->sinkPing.t4)
            - aoo_ntpTimeDuration(event->sinkPing.t2, event->sinkPing.t3);
        sender->metrics.roundTripMilliseconds.store(
            std::isfinite(rtt) && rtt >= 0 ? rtt * 1000.0 : 0,
            std::memory_order_relaxed
        );
        break;
    }
    case kAooEventSinkRemove:
        sender->metrics.peerResponsive.store(0, std::memory_order_release);
        sender->metrics.lastPeerPingNanoseconds.store(0, std::memory_order_relaxed);
        break;
    case kAooEventFrameResend:
        sender->metrics.resentFrames.fetch_add(
            static_cast<uint64_t>(std::max(0, event->frameResend.count)),
            std::memory_order_relaxed
        );
        break;
    default:
        break;
    }
}

void AOO_CALL handleReceiverEvent(
    void *user,
    const AooEvent *event,
    AooThreadLevel
) {
    auto *receiver = static_cast<AOOAppleReceiver *>(user);
    if (!receiver || !event) {
        return;
    }
    switch (event->type) {
    case kAooEventStreamStart: {
        setReceiverSourceEndpoint(receiver, &event->streamStart.endpoint);
        if (!applyReceiverChannelMap(receiver, event->streamStart.metadata)) {
            receiver->metrics.lastError.store(
                kAooErrorBadFormat,
                std::memory_order_relaxed
            );
            receiver->metrics.incompatibleStreams.fetch_add(
                1,
                std::memory_order_relaxed
            );
            receiver->metrics.streamActive.store(0, std::memory_order_release);
            receiver->metrics.streamState.store(
                kAooStreamStateInactive,
                std::memory_order_release
            );
            AooSink_resetSource(receiver->sink, &event->streamStart.endpoint);
            AooSink_uninviteSource(receiver->sink, &event->streamStart.endpoint);
            AooClient_notify(receiver->client);
            setReceiverSourceEndpoint(receiver, nullptr);
            return;
        }
        AooFormatStorage format{};
        format.header.structSize = sizeof(format);
        if (AooSink_control(
                receiver->sink,
                kAooCtlGetFormat,
                reinterpret_cast<AooIntPtr>(&event->streamStart.endpoint),
                &format,
                sizeof(format)
            ) == kAooOk) {
            applyReceiverSourceFormat(receiver, &format.header);
        }
        receiver->metrics.rawRoundTripMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawClockOffsetMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawAOOStreamTimeDeltaMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawStreamTimeDeltaMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawPresentationToSinkCallbackDeltaMilliseconds.store(
            0,
            std::memory_order_relaxed
        );
        receiver->metrics.streamStarts.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    case kAooEventStreamStop:
        if (!receiverSourceEndpointMatches(receiver, &event->streamStop.endpoint)) {
            break;
        }
        receiver->metrics.streamActive.store(0, std::memory_order_release);
        receiver->metrics.streamState.store(
            kAooStreamStateInactive,
            std::memory_order_release
        );
        break;
    case kAooEventStreamState:
        if (!receiverSourceEndpointMatches(receiver, &event->streamState.endpoint)) {
            break;
        }
        {
        const int32_t previousState = receiver->metrics.streamState.load(
            std::memory_order_acquire
        );
        receiver->metrics.streamState.store(
            event->streamState.state,
            std::memory_order_release
        );
        receiver->metrics.streamActive.store(
            event->streamState.state == kAooStreamStateActive ? 1 : 0,
            std::memory_order_release
        );
        switch (event->streamState.state) {
        case kAooStreamStateActive:
            receiver->metrics.streamActiveTransitions.fetch_add(
                1,
                std::memory_order_relaxed
            );
            break;
        case kAooStreamStateBuffering:
            receiver->metrics.streamBufferingTransitions.fetch_add(
                1,
                std::memory_order_relaxed
            );
            if (previousState == kAooStreamStateActive) {
                receiver->metrics.reacquisitions.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }
            break;
        case kAooStreamStateInactive:
            receiver->metrics.streamInactiveTransitions.fetch_add(
                1,
                std::memory_order_relaxed
            );
            break;
        default:
            break;
        }
        }
        break;
    case kAooEventSourceRemove:
        if (!receiverSourceEndpointMatches(receiver, &event->sourceRemove.endpoint)) {
            break;
        }
        receiver->metrics.streamActive.store(0, std::memory_order_release);
        receiver->metrics.streamState.store(
            kAooStreamStateInactive,
            std::memory_order_release
        );
        setReceiverSourceEndpoint(receiver, nullptr);
        applyReceiverChannelMap(receiver, nullptr);
        receiver->metrics.sourceChannelCount.store(0, std::memory_order_relaxed);
        receiver->metrics.sourceSampleRate.store(0, std::memory_order_relaxed);
        receiver->metrics.sourceBlockSize.store(0, std::memory_order_relaxed);
        receiver->metrics.rawRoundTripMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawClockOffsetMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawAOOStreamTimeDeltaMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawStreamTimeDeltaMilliseconds.store(0, std::memory_order_relaxed);
        receiver->metrics.rawPresentationToSinkCallbackDeltaMilliseconds.store(
            0,
            std::memory_order_relaxed
        );
        break;
    case kAooEventSourcePing: {
        if (!receiverSourceEndpointMatches(receiver, &event->sourcePing.endpoint)) {
            break;
        }
        const double rtt = aoo_ntpTimeDuration(
            event->sourcePing.t1,
            event->sourcePing.t4
        ) - aoo_ntpTimeDuration(
            event->sourcePing.t2,
            event->sourcePing.t3
        );
        const double clockOffset = 0.5 * (
            aoo_ntpTimeDuration(event->sourcePing.t1, event->sourcePing.t2)
            + aoo_ntpTimeDuration(event->sourcePing.t4, event->sourcePing.t3)
        );
        if (std::isfinite(rtt) && rtt >= 0 && std::isfinite(clockOffset)) {
            receiver->metrics.rawRoundTripMilliseconds.store(
                rtt * 1000.0,
                std::memory_order_relaxed
            );
            receiver->metrics.rawClockOffsetMilliseconds.store(
                clockOffset * 1000.0,
                std::memory_order_relaxed
            );
            receiver->metrics.pingEvents.fetch_add(1, std::memory_order_release);
        }
        break;
    }
    case kAooEventStreamTime: {
        if (!receiverSourceEndpointMatches(receiver, &event->streamTime.endpoint)) {
            break;
        }
        const double deltaMilliseconds = aoo_ntpTimeDuration(
            event->streamTime.sourceTime,
            event->streamTime.sinkTime
        ) * 1000.0;
        if (std::isfinite(deltaMilliseconds)) {
            receiver->metrics.rawAOOStreamTimeDeltaMilliseconds.store(
                deltaMilliseconds,
                std::memory_order_relaxed
            );
            receiver->metrics.aooStreamTimeEvents.fetch_add(
                1,
                std::memory_order_release
            );
        }
        break;
    }
    case kAooEventFormatChange:
        if (!receiverHasSourceEndpoint(receiver)
            || receiverSourceEndpointMatches(receiver, &event->formatChange.endpoint)) {
            applyReceiverSourceFormat(receiver, event->formatChange.format);
        }
        break;
    case kAooEventStreamLatency:
        if (!receiverSourceEndpointMatches(receiver, &event->streamLatency.endpoint)) {
            break;
        }
        receiver->metrics.sourceLatencyMilliseconds.store(
            std::max(0.0, event->streamLatency.sourceLatency * 1000.0),
            std::memory_order_relaxed
        );
        receiver->metrics.sinkLatencyMilliseconds.store(
            std::max(0.0, event->streamLatency.sinkLatency * 1000.0),
            std::memory_order_relaxed
        );
        receiver->metrics.jitterBufferLatencyMilliseconds.store(
            std::max(0.0, event->streamLatency.bufferLatency * 1000.0),
            std::memory_order_relaxed
        );
        break;
    case kAooEventBufferUnderrun:
        if (!receiverSourceEndpointMatches(receiver, &event->bufferUnderrun.endpoint)) {
            break;
        }
        receiver->metrics.bufferUnderruns.fetch_add(1, std::memory_order_relaxed);
        break;
    case kAooEventBufferOverrun:
        if (!receiverSourceEndpointMatches(receiver, &event->bufferOverrrun.endpoint)) {
            break;
        }
        receiver->metrics.bufferOverruns.fetch_add(1, std::memory_order_relaxed);
        break;
    case kAooEventBlockDrop:
        if (!receiverSourceEndpointMatches(receiver, &event->blockDrop.endpoint)) {
            break;
        }
        receiver->metrics.droppedBlocks.fetch_add(
            static_cast<uint64_t>(std::max(0, event->blockDrop.count)),
            std::memory_order_relaxed
        );
        receiver->metrics.concealments.fetch_add(
            static_cast<uint64_t>(std::max(0, event->blockDrop.count)),
            std::memory_order_relaxed
        );
        break;
    case kAooEventBlockResend:
        if (!receiverSourceEndpointMatches(receiver, &event->blockResend.endpoint)) {
            break;
        }
        receiver->metrics.resentBlocks.fetch_add(
            static_cast<uint64_t>(std::max(0, event->blockResend.count)),
            std::memory_order_relaxed
        );
        break;
    case kAooEventBlockXRun:
        if (!receiverSourceEndpointMatches(receiver, &event->blockXRun.endpoint)) {
            break;
        }
        receiver->metrics.sourceXRuns.fetch_add(
            static_cast<uint64_t>(std::max(0, event->blockXRun.count)),
            std::memory_order_relaxed
        );
        break;
    default:
        break;
    }
}

void startSenderReceiveThread(AOOAppleSender *sender) {
    sender->receiveThread = std::thread([sender] {
        configureNetworkThread();
        const AooError result = AooClient_receive(sender->client, kAooInfinite);
        if (result != kAooOk) {
            sender->metrics.lastError.store(result, std::memory_order_relaxed);
        }
    });
}

void startReceiverNetworkThreads(AOOAppleReceiver *receiver) {
    receiver->sendThread = std::thread([receiver] {
        configureNetworkThread();
        const AooError result = AooClient_send(receiver->client, kAooInfinite);
        if (result != kAooOk) {
            receiver->metrics.lastError.store(result, std::memory_order_relaxed);
        }
    });
    receiver->receiveThread = std::thread([receiver] {
        configureNetworkThread();
        const AooError result = AooClient_receive(receiver->client, kAooInfinite);
        if (result != kAooOk) {
            receiver->metrics.lastError.store(result, std::memory_order_relaxed);
        }
    });
}

void startReceiverEventThread(AOOAppleReceiver *receiver) {
    receiver->eventThreadShouldRun.store(true, std::memory_order_release);
    receiver->eventThread = std::thread([receiver] {
        configureNetworkThread();
        while (receiver->eventThreadShouldRun.load(std::memory_order_acquire)) {
            if (AooSink_eventsAvailable(receiver->sink)) {
                const AooError result = AooSink_pollEvents(receiver->sink);
                if (result != kAooOk) {
                    receiver->metrics.lastError.store(
                        result,
                        std::memory_order_relaxed
                    );
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
}

void startReceiverAdaptiveThread(AOOAppleReceiver *receiver) {
    if ((receiver->transportProfile
            != AOOAppleTransportProfileAdaptiveWireless
         && receiver->transportProfile
            != AOOAppleTransportProfileAutomatic)
        || receiver->adaptiveThread.joinable()) {
        return;
    }
    receiver->adaptiveThreadShouldRun.store(true, std::memory_order_release);
    receiver->adaptiveThread = std::thread([receiver] {
        configureNetworkThread();
        using clock = std::chrono::steady_clock;
        const double blockMilliseconds = static_cast<double>(
            receiver->streamBlockSize
        ) / receiver->sampleRate * 1000.0;
        auto lastInstability = clock::now();
        auto lastAdjustment = clock::time_point::min();
        uint64_t previousObservationCount = 0;
        uint64_t previousConcealments = 0;
        uint64_t previousUnderruns = 0;

        auto roundedTarget = [blockMilliseconds](double milliseconds) {
            const double blocks = std::ceil(
                std::clamp(milliseconds, 8.0, 200.0)
                    / blockMilliseconds - 1.0e-9
            );
            return std::min(200.0, std::max(1.0, blocks) * blockMilliseconds);
        };

        while (receiver->adaptiveThreadShouldRun.load(
                   std::memory_order_acquire
               )) {
            if (receiver->activeTransportProfile.load(
                    std::memory_order_acquire
                ) != AOOAppleTransportProfileAdaptiveWireless) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            AooLowLatencySinkStatistics statistics{};
            if (AooSink_getLowLatencyStatistics(
                    receiver->sink,
                    &statistics
                ) == kAooOk
                && statistics.arrivalObservationCount
                    != previousObservationCount) {
                previousObservationCount = statistics.arrivalObservationCount;
                const auto now = clock::now();
                const uint64_t concealments = receiver->metrics.concealments.load(
                    std::memory_order_relaxed
                );
                const uint64_t underruns = receiver->metrics.bufferUnderruns.load(
                    std::memory_order_relaxed
                );
                const bool hadInstability = concealments != previousConcealments
                    || underruns != previousUnderruns;
                previousConcealments = concealments;
                previousUnderruns = underruns;

                const double jitterMilliseconds = std::max(
                    0.0,
                    statistics.p99ArrivalResidual * 1000.0
                );
                const double currentTarget = receiver->targetLatencySeconds.load(
                    std::memory_order_acquire
                ) * 1000.0;
                const double requiredTarget = roundedTarget(
                    jitterMilliseconds + 2 * blockMilliseconds
                );
                const bool adjustmentAllowed = lastAdjustment
                        == clock::time_point::min()
                    || now - lastAdjustment >= std::chrono::seconds(30);

                if (hadInstability || requiredTarget > currentTarget) {
                    lastInstability = now;
                }
                double nextTarget = currentTarget;
                if (requiredTarget > currentTarget) {
                    nextTarget = requiredTarget;
                } else if (adjustmentAllowed
                           && now - lastInstability >= std::chrono::seconds(30)
                           && currentTarget - jitterMilliseconds
                               >= 3 * blockMilliseconds) {
                    nextTarget = roundedTarget(
                        std::max(8.0, currentTarget - blockMilliseconds)
                    );
                }

                if (std::abs(nextTarget - currentTarget) > 1.0e-6) {
                    const AooError result = AooSink_setLowLatencyTarget(
                        receiver->sink,
                        static_cast<AooUInt32>(std::llround(
                            nextTarget * 0.001 * receiver->sampleRate
                        ))
                    );
                    if (result == kAooOk) {
                        receiver->targetLatencySeconds.store(
                            nextTarget * 0.001,
                            std::memory_order_release
                        );
                        receiver->metrics.adaptiveAdjustments.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                        lastAdjustment = now;
                    } else {
                        receiver->metrics.lastError.store(
                            result,
                            std::memory_order_relaxed
                        );
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
}

AooError enqueueSenderProcessBlock(
    AOOAppleSender *sender,
    const float *planarBaseAddress,
    int32_t channelStride,
    int32_t frameOffset,
    int32_t frameCount,
    bool isSilence,
    uint64_t sourceSamplePosition,
    AooNtpTime sourceNtpTime,
    AooNtpTime sourcePresentationNtpTime
) {
    AooError result = kAooOk;
    int32_t consumedFrameCount = 0;
    const uint64_t generation = sender->streamGeneration.load(
        std::memory_order_acquire
    );
    while (consumedFrameCount < frameCount) {
        if (sender->pendingFrameCount > 0
            && sender->pendingGeneration != generation) {
            sender->pendingFrameCount = 0;
        }
        if (sender->pendingFrameCount == 0) {
            sender->pendingGeneration = generation;
            sender->pendingIsSilence = true;
            sender->pendingSourceSamplePosition = sourceSamplePosition
                + static_cast<uint64_t>(consumedFrameCount);
            sender->pendingSourceNtpTime = AOOAppleNTPTimeOffsetFrames(
                sourceNtpTime,
                consumedFrameCount,
                sender->sampleRate
            );
            sender->pendingSourcePresentationNtpTime = AOOAppleNTPTimeOffsetFrames(
                sourcePresentationNtpTime,
                consumedFrameCount,
                sender->sampleRate
            );
        }

        const int32_t copyFrameCount = std::min(
            frameCount - consumedFrameCount,
            sender->streamBlockSize - sender->pendingFrameCount
        );
        const size_t byteCount = static_cast<size_t>(copyFrameCount) * sizeof(AooSample);
        for (int32_t channel = 0; channel < sender->channelCount; ++channel) {
            AooSample *destination = sender->pendingChannelPointers[channel]
                + sender->pendingFrameCount;
            if (isSilence) {
                std::memset(destination, 0, byteCount);
            } else {
                const int32_t sourceChannel = sender->sourceChannelIndices[channel];
                const float *source = planarBaseAddress
                    + sourceChannel * channelStride
                    + frameOffset
                    + consumedFrameCount;
                for (int32_t frame = 0; frame < copyFrameCount; ++frame) {
                    destination[frame] = finiteSample(source[frame]);
                }
            }
        }
        sender->pendingIsSilence = sender->pendingIsSilence && isSilence;
        sender->pendingFrameCount += copyFrameCount;
        consumedFrameCount += copyFrameCount;

        if (sender->pendingFrameCount != sender->streamBlockSize) {
            continue;
        }

        const uint64_t writeIndex = sender->processWriteIndex.load(std::memory_order_relaxed);
        const uint64_t readIndex = sender->processReadIndex.load(std::memory_order_acquire);
        if (writeIndex - readIndex >= sender->processQueue.size()) {
            sender->metrics.handoffDrops.fetch_add(1, std::memory_order_relaxed);
            const uint64_t gapWriteIndex = sender->processGapWriteIndex.load(
                std::memory_order_relaxed
            );
            const uint64_t gapReadIndex = sender->processGapReadIndex.load(
                std::memory_order_acquire
            );
            if (gapWriteIndex - gapReadIndex < sender->processGapQueue.size()) {
                auto& gap = sender->processGapQueue[
                    gapWriteIndex % sender->processGapQueue.size()
                ];
                gap.sourceSamplePosition = sender->pendingSourceSamplePosition;
                gap.sourceNtpTime = sender->pendingSourceNtpTime;
                gap.sourcePresentationNtpTime =
                    sender->pendingSourcePresentationNtpTime;
                gap.enqueueNtpTime = aoo_getCurrentNtpTime();
                gap.generation = sender->pendingGeneration;
                sender->processGapWriteIndex.store(
                    gapWriteIndex + 1,
                    std::memory_order_release
                );
            }
            result = kAooErrorWouldBlock;
        } else {
            auto& block = sender->processQueue[writeIndex % sender->processQueue.size()];
            block.frameCount = sender->streamBlockSize;
            block.isSilence = sender->pendingIsSilence;
            block.sourceSamplePosition = sender->pendingSourceSamplePosition;
            block.sourceNtpTime = sender->pendingSourceNtpTime;
            block.sourcePresentationNtpTime = sender->pendingSourcePresentationNtpTime;
            block.enqueueNtpTime = aoo_getCurrentNtpTime();
            block.generation = sender->pendingGeneration;
            if (!block.isSilence) {
                const size_t blockByteCount = static_cast<size_t>(
                    sender->streamBlockSize
                ) * sizeof(AooSample);
                for (int32_t channel = 0; channel < sender->channelCount; ++channel) {
                    std::memcpy(
                        block.channelPointers[channel],
                        sender->pendingChannelPointers[channel],
                        blockByteCount
                    );
                }
            }
            sender->processWriteIndex.store(writeIndex + 1, std::memory_order_release);
        }
        sender->pendingFrameCount = 0;
    }
    return result;
}

bool processNextSenderBlock(AOOAppleSender *sender) {
    const uint64_t readIndex = sender->processReadIndex.load(std::memory_order_relaxed);
    const uint64_t writeIndex = sender->processWriteIndex.load(std::memory_order_acquire);
    const uint64_t gapReadIndex = sender->processGapReadIndex.load(
        std::memory_order_relaxed
    );
    const uint64_t gapWriteIndex = sender->processGapWriteIndex.load(
        std::memory_order_acquire
    );
    const bool hasBlock = readIndex != writeIndex;
    const bool hasGap = gapReadIndex != gapWriteIndex;
    if (!hasBlock && !hasGap) {
        return false;
    }

    SenderProcessBlock *block = hasBlock
        ? &sender->processQueue[readIndex % sender->processQueue.size()]
        : nullptr;
    SenderProcessGap *gap = hasGap
        ? &sender->processGapQueue[gapReadIndex % sender->processGapQueue.size()]
        : nullptr;
    const bool processGap = gap
        && (!block || gap->sourceSamplePosition < block->sourceSamplePosition);
    const uint64_t sourceSamplePosition = processGap
        ? gap->sourceSamplePosition
        : block->sourceSamplePosition;
    const AooNtpTime sourceNtpTime = processGap
        ? gap->sourceNtpTime
        : block->sourceNtpTime;
    const AooNtpTime sourcePresentationNtpTime = processGap
        ? gap->sourcePresentationNtpTime
        : block->sourcePresentationNtpTime;
    const AooNtpTime enqueueNtpTime = processGap
        ? gap->enqueueNtpTime
        : block->enqueueNtpTime;
    const uint64_t generation = processGap
        ? gap->generation
        : block->generation;
    const bool staleGeneration = generation != sender->streamGeneration.load(
        std::memory_order_acquire
    );
    AooSample **channels = processGap || block->isSilence
        ? sender->silencePointers.data()
        : block->channelPointers.data();

    if (!staleGeneration
        && sender->metrics.enabled.load(std::memory_order_acquire) != 0
        && sender->metrics.hasDestination.load(std::memory_order_acquire) != 0) {
        const AooNtpTime processNtpTime = aoo_getCurrentNtpTime();
        const double handoffLatency = aoo_ntpTimeDuration(
            enqueueNtpTime,
            processNtpTime
        ) * 1000.0;
        if (std::isfinite(handoffLatency) && handoffLatency >= 0) {
            sender->metrics.handoffLatencyMilliseconds.store(
                handoffLatency,
                std::memory_order_relaxed
            );
            accumulateMaximum(
                sender->metrics.maximumHandoffLatencyMilliseconds,
                handoffLatency
            );
        }
        const AooNtpTime previousPresentationMessageTime =
            sender->lastPresentationMessageNtpTime.load(std::memory_order_acquire);
        const bool shouldSendPresentationTimestamp = previousPresentationMessageTime == 0
            || aoo_ntpTimeDuration(
                previousPresentationMessageTime,
                sourceNtpTime
            ) >= kStreamTimeSendIntervalSeconds;
        if (shouldSendPresentationTimestamp) {
            std::array<AooByte, kSourceTimingTimestampByteCount> timestampBytes{};
            encodeUInt64(sourceSamplePosition, timestampBytes.data());
            encodeUInt64(
                sourceNtpTime,
                timestampBytes.data() + kEncodedNtpTimestampByteCount
            );
            encodeUInt64(
                sourcePresentationNtpTime,
                timestampBytes.data() + kEncodedNtpTimestampByteCount * 2
            );
            const AooStreamMessage message = {
                0,
                0,
                kSourceTimingTimestampMessageType,
                kSourceTimingTimestampByteCount,
                timestampBytes.data(),
            };
            if (AooSource_addStreamMessage(sender->source, &message) == kAooOk) {
                sender->lastPresentationMessageNtpTime.store(
                    sourceNtpTime,
                    std::memory_order_release
                );
            }
        }
        const AooError result = AooSource_processLowLatency(
            sender->source,
            channels,
            sender->streamBlockSize,
            sourceNtpTime,
            sourceSamplePosition
        );
        const double presentationLead = aoo_ntpTimeDuration(
            sourceNtpTime,
            sourcePresentationNtpTime
        ) * 1000.0;
        if (std::isfinite(presentationLead)) {
            sender->metrics.sourcePresentationLeadMilliseconds.store(
                presentationLead,
                std::memory_order_relaxed
            );
        }
        sender->metrics.processCalls.fetch_add(1, std::memory_order_relaxed);
        sender->metrics.processedFrames.fetch_add(
            static_cast<uint64_t>(sender->streamBlockSize),
            std::memory_order_relaxed
        );
        if (result != kAooOk && result != kAooErrorIdle && result != kAooErrorWouldBlock) {
            sender->metrics.processErrors.fetch_add(1, std::memory_order_relaxed);
            sender->metrics.lastError.store(result, std::memory_order_relaxed);
        }
        const AooError sendResult = AooClient_send(sender->client, 0);
        if (sendResult != kAooOk && sendResult != kAooErrorWouldBlock) {
            sender->metrics.lastError.store(sendResult, std::memory_order_relaxed);
        }
    }
    if (processGap) {
        sender->processGapReadIndex.store(
            gapReadIndex + 1,
            std::memory_order_release
        );
    } else {
        sender->processReadIndex.store(readIndex + 1, std::memory_order_release);
    }
    return true;
}

void startSenderProcessThread(AOOAppleSender *sender) {
    if (sender->processThread.joinable()) {
        return;
    }
    sender->processReadIndex.store(
        sender->processWriteIndex.load(std::memory_order_acquire),
        std::memory_order_release
    );
    sender->processGapReadIndex.store(
        sender->processGapWriteIndex.load(std::memory_order_acquire),
        std::memory_order_release
    );
    sender->streamGeneration.fetch_add(1, std::memory_order_acq_rel);
    sender->processThreadShouldRun.store(true, std::memory_order_release);
    sender->processThread = std::thread([sender] {
        configureNetworkThread();
        const auto blockDuration = std::chrono::duration<double>(
            static_cast<double>(sender->streamBlockSize) / sender->sampleRate
        );
        auto nextDeadline = std::chrono::steady_clock::now();
        while (sender->processThreadShouldRun.load(std::memory_order_acquire)) {
            const bool processed = processNextSenderBlock(sender);
            if (!processed) {
                const AooError result = AooClient_send(sender->client, 0);
                if (result != kAooOk && result != kAooErrorWouldBlock) {
                    sender->metrics.lastError.store(result, std::memory_order_relaxed);
                }
            }
            nextDeadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                blockDuration
            );
            const auto now = std::chrono::steady_clock::now();
            if (nextDeadline > now) {
                std::this_thread::sleep_until(nextDeadline);
            } else {
                nextDeadline = now;
            }
        }
    });
}

void stopSenderProcessThread(AOOAppleSender *sender) {
    sender->processThreadShouldRun.store(false, std::memory_order_release);
    if (sender->processThread.joinable()) {
        sender->processThread.join();
    }
    sender->processReadIndex.store(
        sender->processWriteIndex.load(std::memory_order_acquire),
        std::memory_order_release
    );
    sender->processGapReadIndex.store(
        sender->processGapWriteIndex.load(std::memory_order_acquire),
        std::memory_order_release
    );
}

} // namespace

const char *AOOAppleErrorString(int32_t errorCode) {
    return aoo_strerror(static_cast<AooError>(errorCode));
}

int32_t AOOAppleLastSocketErrorCode(void) {
    AooInt32 errorCode = 0;
    return aoo_getLastSocketError(&errorCode, nullptr, nullptr) == kAooOk
        ? errorCode
        : 0;
}

uint64_t AOOAppleCurrentNTPTime(void) {
    return aoo_getCurrentNtpTime();
}

uint64_t AOOAppleNTPTimeOffsetFrames(
    uint64_t timestamp,
    int64_t frameOffset,
    double sampleRate
) {
    if (timestamp == 0 || frameOffset == 0) {
        return timestamp;
    }
    const double resolvedSampleRate = clampedSampleRate(sampleRate);
    const AooNtpTime duration = aoo_ntpTimeFromSeconds(
        std::abs(static_cast<double>(frameOffset)) / resolvedSampleRate
    );
    return frameOffset > 0 ? timestamp + duration : timestamp - duration;
}

uint64_t AOOAppleNTPTimeOffsetNanoseconds(
    uint64_t timestamp,
    int64_t nanosecondOffset
) {
    if (timestamp == 0 || nanosecondOffset == 0) {
        return timestamp;
    }
    const double seconds = std::abs(static_cast<double>(nanosecondOffset)) * 1e-9;
    const AooNtpTime duration = aoo_ntpTimeFromSeconds(seconds);
    return nanosecondOffset > 0 ? timestamp + duration : timestamp - duration;
}

AOOAppleSender *AOOAppleSenderCreate(
    int32_t localPort,
    int32_t sourceID,
    int32_t channelCount,
    const int32_t *sourceChannelIndices,
    int32_t sourceChannelIndexCount,
    double sampleRate,
    int32_t maximumBlockSize,
    int32_t streamBlockSize,
    int32_t packetSize,
    double targetLatencyMilliseconds,
    int32_t transportProfile,
    int32_t pcmFormat,
    uint32_t capabilities,
    int32_t *errorCode
) {
    if (errorCode) {
        *errorCode = kAooOk;
    }
    if (channelCount <= 0 || channelCount > kMaximumChannelCount
        || !sourceChannelIndices || sourceChannelIndexCount != channelCount
        || (transportProfile != AOOAppleTransportProfileDeterministicWired
            && transportProfile != AOOAppleTransportProfileAdaptiveWireless)
        || (pcmFormat != AOOApplePCMFormatFloat32
            && pcmFormat != AOOApplePCMFormatInt24)
        || (transportProfile == AOOAppleTransportProfileDeterministicWired
            && pcmFormat != AOOApplePCMFormatFloat32)
        || (transportProfile == AOOAppleTransportProfileAdaptiveWireless
            && pcmFormat != AOOApplePCMFormatInt24)
        || (capabilities & (
                kAooLowLatencyCapabilityAbsoluteSamplePosition
                | kAooLowLatencyCapabilityChannelMap
                | kAooLowLatencyCapabilityDynamicResampling
            )) != (
                kAooLowLatencyCapabilityAbsoluteSamplePosition
                | kAooLowLatencyCapabilityChannelMap
                | kAooLowLatencyCapabilityDynamicResampling
            )
        || (transportProfile == AOOAppleTransportProfileAdaptiveWireless
            && !(capabilities & kAooLowLatencyCapabilityDeadlineResend))) {
        if (errorCode) {
            *errorCode = kAooErrorBadArgument;
        }
        return nullptr;
    }
    std::array<bool, kMaximumChannelCount> usedSourceChannels{};
    for (int32_t channel = 0; channel < channelCount; ++channel) {
        const int32_t sourceChannel = sourceChannelIndices[channel];
        if (sourceChannel < 0 || sourceChannel >= kMaximumChannelCount
            || usedSourceChannels[sourceChannel]) {
            if (errorCode) {
                *errorCode = kAooErrorBadArgument;
            }
            return nullptr;
        }
        usedSourceChannels[sourceChannel] = true;
    }
    if (!retainAOO()) {
        if (errorCode) {
            *errorCode = kAooErrorSystem;
        }
        return nullptr;
    }

    auto sender = std::unique_ptr<AOOAppleSender>(
        new (std::nothrow) AOOAppleSender()
    );
    if (!sender) {
        releaseAOO();
        if (errorCode) {
            *errorCode = kAooErrorOutOfMemory;
        }
        return nullptr;
    }

    sender->localPort = clampedSenderPort(localPort);
    sender->channelCount = channelCount;
    sender->sampleRate = static_cast<int32_t>(std::lround(clampedSampleRate(sampleRate)));
    sender->maximumBlockSize = clampedBlockSize(maximumBlockSize);
    sender->streamBlockSize = std::clamp(
        streamBlockSize,
        16,
        sender->maximumBlockSize
    );
    sender->packetSize = clampedPacketSize(packetSize);
    sender->transportProfile = transportProfile;
    sender->pcmFormat = pcmFormat;
    AooLowLatencyStreamConfiguration streamConfiguration{};
    streamConfiguration.protocolVersion = AOO_LOW_LATENCY_PROTOCOL_VERSION;
    streamConfiguration.profile = static_cast<AooByte>(transportProfile);
    streamConfiguration.pcmFormat = static_cast<AooByte>(pcmFormat);
    streamConfiguration.channelCount = static_cast<AooUInt16>(sender->channelCount);
    streamConfiguration.blockFrames = static_cast<AooUInt16>(sender->streamBlockSize);
    streamConfiguration.datagramBytes = static_cast<AooUInt16>(sender->packetSize);
    streamConfiguration.channelMapCount = static_cast<AooUInt16>(sender->channelCount);
    streamConfiguration.sampleRate = static_cast<AooUInt32>(sender->sampleRate);
    streamConfiguration.targetLatencyFrames = static_cast<AooUInt32>(std::max(
        sender->streamBlockSize,
        static_cast<int32_t>(std::ceil(
            clampedLatencySeconds(targetLatencyMilliseconds) * sender->sampleRate
        ))
    ));
    streamConfiguration.capabilities = capabilities;
    for (int32_t channel = 0; channel < sender->channelCount; ++channel) {
        const int32_t sourceChannel = sourceChannelIndices[channel];
        sender->sourceChannelIndices[channel] = sourceChannel;
        sender->requiredSourceChannelCount = std::max(
            sender->requiredSourceChannelCount,
            sourceChannel + 1
        );
        streamConfiguration.channelMap[channel] = static_cast<AooByte>(sourceChannel);
    }
    if (aoo_lowLatencyStreamConfigurationEncode(
            &streamConfiguration,
            sender->streamMetadata.data(),
            sender->streamMetadata.size()
        ) != kAooOk) {
        if (errorCode) {
            *errorCode = kAooErrorBadArgument;
        }
        return nullptr;
    }
    sender->streamMetadataSize = AOO_LOW_LATENCY_STREAM_CONFIGURATION_SIZE;
    sender->silenceStorage.assign(
        static_cast<size_t>(sender->channelCount * sender->streamBlockSize),
        0
    );
    sender->pendingStorage.assign(
        static_cast<size_t>(sender->channelCount * sender->streamBlockSize),
        0
    );
    for (int32_t channel = 0; channel < sender->channelCount; ++channel) {
        sender->silencePointers[channel] = sender->silenceStorage.data()
            + channel * sender->streamBlockSize;
        sender->pendingChannelPointers[channel] = sender->pendingStorage.data()
            + channel * sender->streamBlockSize;
    }
    const uint64_t maximumBlocksPerCall = static_cast<uint64_t>(
        (sender->maximumBlockSize + sender->streamBlockSize - 1)
            / sender->streamBlockSize
    );
    sender->processQueue.resize(std::max(
        kMinimumSenderProcessQueueCapacity,
        maximumBlocksPerCall + kMinimumSenderProcessQueueCapacity
    ));
    for (auto& block : sender->processQueue) {
        block.storage.assign(
            static_cast<size_t>(sender->channelCount * sender->streamBlockSize),
            0
        );
        for (int32_t channel = 0; channel < sender->channelCount; ++channel) {
            block.channelPointers[channel] = block.storage.data()
                + channel * sender->streamBlockSize;
        }
    }

    sender->source = AooSource_new(sourceID);
    sender->client = AooClient_new();
    if (!sender->source || !sender->client) {
        if (errorCode) {
            *errorCode = kAooErrorOutOfMemory;
        }
        return nullptr;
    }

    AooError result = AooSource_setup(
        sender->source,
        sender->channelCount,
        sender->sampleRate,
        sender->streamBlockSize,
        kAooFixedBlockSize
    );
    if (result != kAooOk) {
        if (errorCode) {
            *errorCode = result;
        }
        return nullptr;
    }

    AooFormatPcm format;
    AooFormatPcm_init(
        &format,
        sender->channelCount,
        sender->sampleRate,
        sender->streamBlockSize,
        sender->pcmFormat == AOOApplePCMFormatInt24
            ? kAooPcmInt24
            : kAooPcmFloat32
    );
    result = AooSource_setFormat(sender->source, &format.header);
    if (result == kAooOk) {
        result = AooSource_setDynamicResampling(sender->source, kAooTrue);
    }
    if (result == kAooOk) {
        result = AooSource_setPacketSize(sender->source, sender->packetSize);
    }
    if (result == kAooOk) {
        result = AooSource_setPingInterval(sender->source, kPingIntervalSeconds);
    }
    if (result == kAooOk) {
        result = AooSource_setStreamTimeSendInterval(
            sender->source,
            kStreamTimeSendIntervalSeconds
        );
    }
    if (result == kAooOk) {
        result = AooSource_setResendBufferSize(
            sender->source,
            sender->transportProfile == AOOAppleTransportProfileDeterministicWired
                ? 0.0
                : 0.2
        );
    }
    if (result == kAooOk) {
        result = AooSource_setRedundancy(sender->source, 1);
    }
    if (result == kAooOk) {
        result = AooSource_setEventHandler(
            sender->source,
            handleSenderEvent,
            sender.get(),
            kAooEventModeCallback
        );
    }
    if (result != kAooOk) {
        if (errorCode) {
            *errorCode = result;
        }
        return nullptr;
    }

    AooClientSettings settings;
    settings.portNumber = static_cast<AooUInt16>(sender->localPort);
    result = AooClient_setup(sender->client, &settings);
    if (result == kAooOk) {
        sender->localPort = settings.portNumber;
        sender->socketType = settings.socketType;
        result = AooClient_addSource(sender->client, sender->source);
    }
    if (result != kAooOk) {
        if (errorCode) {
            *errorCode = result;
        }
        return nullptr;
    }

    sender->metrics.ready.store(1, std::memory_order_release);
    startSenderReceiveThread(sender.get());
    return sender.release();
}

void AOOAppleSenderDestroy(AOOAppleSender *sender) {
    delete sender;
}

int32_t AOOAppleSenderSetDestination(
    AOOAppleSender *sender,
    const char *host,
    int32_t port,
    int32_t sinkID
) {
    if (!sender || !sender->source || !host || host[0] == '\0') {
        return kAooErrorBadArgument;
    }
    std::lock_guard<std::mutex> lock(sender->destinationMutex);
    AooSockAddrStorage address;
    AooAddrSize addressSize = sizeof(address);
    const AooError result = aoo_resolveIpEndpoint(
        host,
        clampedPort(port, 9999),
        sender->socketType,
        &address,
        &addressSize
    );
    if (result != kAooOk) {
        sender->metrics.lastError.store(result, std::memory_order_relaxed);
        return result;
    }

    const bool wasEnabled = sender->metrics.enabled.exchange(
        0,
        std::memory_order_acq_rel
    ) != 0;
    if (wasEnabled) {
        stopSenderProcessThread(sender);
        AooSource_stopStream(sender->source, 0);
    }

    AooSource_removeAll(sender->source);
    const AooEndpoint endpoint = {&address, addressSize, sinkID};
    const AooError addResult = AooSource_addSink(sender->source, &endpoint, kAooTrue);
    if (addResult != kAooOk) {
        sender->metrics.lastError.store(addResult, std::memory_order_relaxed);
        sender->metrics.hasDestination.store(0, std::memory_order_release);
        return addResult;
    }
    sender->metrics.hasDestination.store(1, std::memory_order_release);
    sender->metrics.peerResponsive.store(0, std::memory_order_release);
    sender->metrics.lastPeerPingNanoseconds.store(0, std::memory_order_relaxed);
    sender->metrics.lastError.store(kAooOk, std::memory_order_relaxed);
    if (wasEnabled) {
        const AooError startResult = startSenderStream(sender);
        if (startResult != kAooOk) {
            sender->metrics.lastError.store(startResult, std::memory_order_relaxed);
            return startResult;
        }
        startSenderProcessThread(sender);
        sender->metrics.enabled.store(1, std::memory_order_release);
    }
    AooClient_notify(sender->client);
    return kAooOk;
}

void AOOAppleSenderClearDestination(AOOAppleSender *sender) {
    if (!sender || !sender->source) {
        return;
    }
    std::lock_guard<std::mutex> lock(sender->destinationMutex);
    const bool wasEnabled = sender->metrics.enabled.exchange(
        0,
        std::memory_order_acq_rel
    ) != 0;
    if (wasEnabled) {
        stopSenderProcessThread(sender);
    }
    AooSource_stopStream(sender->source, 0);
    AooSource_removeAll(sender->source);
    sender->metrics.hasDestination.store(0, std::memory_order_release);
    sender->metrics.peerResponsive.store(0, std::memory_order_release);
    sender->metrics.lastPeerPingNanoseconds.store(0, std::memory_order_relaxed);
    sender->metrics.enabled.store(wasEnabled ? 1 : 0, std::memory_order_release);
    AooClient_notify(sender->client);
}

int32_t AOOAppleSenderSetEnabled(AOOAppleSender *sender, int32_t enabled) {
    if (!sender || !sender->source) {
        return kAooErrorBadArgument;
    }
    const bool shouldEnable = enabled != 0;
    const bool wasEnabled = sender->metrics.enabled.load(std::memory_order_acquire) != 0;
    if (shouldEnable == wasEnabled) {
        return kAooOk;
    }
    AooError result = kAooOk;
    if (shouldEnable) {
        if (sender->metrics.hasDestination.load(std::memory_order_acquire) != 0) {
            result = startSenderStream(sender);
        }
        if (result == kAooOk) {
            if (sender->metrics.hasDestination.load(std::memory_order_acquire) != 0) {
                startSenderProcessThread(sender);
            }
            sender->metrics.enabled.store(1, std::memory_order_release);
        }
    } else {
        sender->metrics.enabled.store(0, std::memory_order_release);
        stopSenderProcessThread(sender);
        result = AooSource_stopStream(sender->source, 0);
    }
    sender->metrics.lastError.store(result, std::memory_order_relaxed);
    AooClient_notify(sender->client);
    return result;
}

int32_t AOOAppleSenderProcessPlanarAtTime(
    AOOAppleSender *sender,
    const float *planarBaseAddress,
    int32_t channelStride,
    int32_t frameOffset,
    int32_t channelCount,
    int32_t frameCount,
    uint64_t sourceSamplePosition,
    uint64_t sourceNtpTime,
    uint64_t sourcePresentationNtpTime
) {
    if (!sender || !planarBaseAddress || channelStride <= 0 || frameOffset < 0
        || frameCount <= 0 || channelCount < sender->requiredSourceChannelCount
        || frameOffset + frameCount > channelStride || sourceNtpTime == 0
        || sourcePresentationNtpTime == 0) {
        return kAooErrorBadArgument;
    }
    if (sender->metrics.enabled.load(std::memory_order_acquire) == 0
        || sender->metrics.hasDestination.load(std::memory_order_acquire) == 0) {
        return kAooOk;
    }
    if (frameCount > sender->maximumBlockSize) {
        return kAooErrorBadArgument;
    }
    return enqueueSenderProcessBlock(
        sender,
        planarBaseAddress,
        channelStride,
        frameOffset,
        frameCount,
        false,
        sourceSamplePosition,
        sourceNtpTime,
        sourcePresentationNtpTime
    );
}

int32_t AOOAppleSenderProcessSilenceAtTime(
    AOOAppleSender *sender,
    int32_t frameCount,
    uint64_t sourceSamplePosition,
    uint64_t sourceNtpTime,
    uint64_t sourcePresentationNtpTime
) {
    if (!sender || frameCount <= 0 || frameCount > sender->maximumBlockSize
        || sourceNtpTime == 0 || sourcePresentationNtpTime == 0) {
        return kAooErrorBadArgument;
    }
    if (sender->metrics.enabled.load(std::memory_order_acquire) == 0
        || sender->metrics.hasDestination.load(std::memory_order_acquire) == 0) {
        return kAooOk;
    }
    return enqueueSenderProcessBlock(
        sender,
        nullptr,
        0,
        0,
        frameCount,
        true,
        sourceSamplePosition,
        sourceNtpTime,
        sourcePresentationNtpTime
    );
}

void AOOAppleSenderGetStatus(
    const AOOAppleSender *sender,
    AOOAppleSenderStatus *status
) {
    if (!status) {
        return;
    }
    std::memset(status, 0, sizeof(*status));
    if (!sender) {
        return;
    }
    status->isReady = sender->metrics.ready.load(std::memory_order_acquire);
    status->isEnabled = sender->metrics.enabled.load(std::memory_order_acquire);
    status->hasDestination = sender->metrics.hasDestination.load(std::memory_order_acquire);
    const int64_t lastPeerPing = sender->metrics.lastPeerPingNanoseconds.load(
        std::memory_order_relaxed
    );
    const bool pingIsFresh = lastPeerPing > 0
        && monotonicNanoseconds() - lastPeerPing <= 2000000000LL;
    status->peerResponsive = pingIsFresh
        ? sender->metrics.peerResponsive.load(std::memory_order_acquire)
        : 0;
    status->localPort = sender->localPort;
    status->channelCount = sender->channelCount;
    status->sampleRate = sender->sampleRate;
    status->streamBlockSize = sender->streamBlockSize;
    status->packetSize = sender->packetSize;
    status->transportProfile = sender->transportProfile;
    status->pcmFormat = sender->pcmFormat;
    status->lastError = sender->metrics.lastError.load(std::memory_order_relaxed);
    status->processCallCount = sender->metrics.processCalls.load(std::memory_order_relaxed);
    status->processedFrameCount = sender->metrics.processedFrames.load(std::memory_order_relaxed);
    status->processErrorCount = sender->metrics.processErrors.load(std::memory_order_relaxed);
    status->handoffDropCount = sender->metrics.handoffDrops.load(std::memory_order_relaxed);
    status->resentFrameCount = sender->metrics.resentFrames.load(std::memory_order_relaxed);
    status->pingEventCount = sender->metrics.pingEvents.load(std::memory_order_relaxed);
    status->handoffLatencyMilliseconds = sender->metrics.handoffLatencyMilliseconds.load(std::memory_order_relaxed);
    status->maximumHandoffLatencyMilliseconds = sender->metrics.maximumHandoffLatencyMilliseconds.load(std::memory_order_relaxed);
    status->sourcePresentationLeadMilliseconds = sender->metrics.sourcePresentationLeadMilliseconds.load(std::memory_order_relaxed);
    status->roundTripMilliseconds = sender->metrics.roundTripMilliseconds.load(std::memory_order_relaxed);
    status->packetLoss = sender->metrics.packetLoss.load(std::memory_order_relaxed);
    double realSampleRate = 0;
    if (AooSource_control(
            const_cast<AooSource *>(sender->source),
            kAooCtlGetRealSampleRate,
            0,
            &realSampleRate,
            sizeof(realSampleRate)
        ) == kAooOk && std::isfinite(realSampleRate)) {
        status->realSampleRate = realSampleRate;
    }
}

AOOAppleReceiver *AOOAppleReceiverCreate(
    int32_t localPort,
    int32_t sinkID,
    int32_t outputChannelCount,
    int32_t sourceChannelCount,
    const int32_t *sourceChannelIndices,
    int32_t sourceChannelIndexCount,
    double sampleRate,
    int32_t maximumBlockSize,
    int32_t streamBlockSize,
    int32_t fixedBlockSizeEnabled,
    double latencyMilliseconds,
    int32_t packetSize,
    int32_t transportProfile,
    int32_t pcmFormat,
    uint32_t capabilities,
    int32_t *errorCode
) {
    if (errorCode) {
        *errorCode = kAooOk;
    }
    if (outputChannelCount <= 0 || outputChannelCount > kMaximumChannelCount
        || sourceChannelCount <= 0 || sourceChannelCount > outputChannelCount
        || !sourceChannelIndices || sourceChannelIndexCount != sourceChannelCount
        || (transportProfile != AOOAppleTransportProfileAutomatic
            && transportProfile != AOOAppleTransportProfileDeterministicWired
            && transportProfile != AOOAppleTransportProfileAdaptiveWireless)
        || (transportProfile != AOOAppleTransportProfileAutomatic
            && pcmFormat != AOOApplePCMFormatFloat32
            && pcmFormat != AOOApplePCMFormatInt24)
        || (transportProfile == AOOAppleTransportProfileDeterministicWired
            && pcmFormat != AOOApplePCMFormatFloat32)
        || (transportProfile == AOOAppleTransportProfileAdaptiveWireless
            && pcmFormat != AOOApplePCMFormatInt24)
        || (capabilities & (
                kAooLowLatencyCapabilityAbsoluteSamplePosition
                | kAooLowLatencyCapabilityChannelMap
                | kAooLowLatencyCapabilityDynamicResampling
            )) != (
                kAooLowLatencyCapabilityAbsoluteSamplePosition
                | kAooLowLatencyCapabilityChannelMap
                | kAooLowLatencyCapabilityDynamicResampling
            )
        || (transportProfile == AOOAppleTransportProfileAdaptiveWireless
            && !(capabilities & kAooLowLatencyCapabilityDeadlineResend))) {
        if (errorCode) {
            *errorCode = kAooErrorBadArgument;
        }
        return nullptr;
    }
    std::array<bool, kMaximumChannelCount> usedSourceChannels{};
    for (int32_t channel = 0; channel < sourceChannelCount; ++channel) {
        const int32_t sourceChannel = sourceChannelIndices[channel];
        if (sourceChannel < 0 || sourceChannel >= outputChannelCount
            || usedSourceChannels[sourceChannel]) {
            if (errorCode) {
                *errorCode = kAooErrorBadArgument;
            }
            return nullptr;
        }
        usedSourceChannels[sourceChannel] = true;
    }
    if (!retainAOO()) {
        if (errorCode) {
            *errorCode = kAooErrorSystem;
        }
        return nullptr;
    }
    auto receiver = std::unique_ptr<AOOAppleReceiver>(
        new (std::nothrow) AOOAppleReceiver()
    );
    if (!receiver) {
        releaseAOO();
        if (errorCode) {
            *errorCode = kAooErrorOutOfMemory;
        }
        return nullptr;
    }

    receiver->localPort = clampedPort(localPort, 9999);
    receiver->channelCount = clampedChannelCount(outputChannelCount);
    receiver->sampleRate = static_cast<int32_t>(std::lround(clampedSampleRate(sampleRate)));
    receiver->maximumBlockSize = clampedBlockSize(maximumBlockSize);
    receiver->streamBlockSize = clampedBlockSize(streamBlockSize);
    receiver->fixedBlockSizeEnabled = fixedBlockSizeEnabled != 0;
    receiver->packetSize = clampedPacketSize(packetSize);
    receiver->transportProfile = transportProfile;
    receiver->pcmFormat = pcmFormat;
    receiver->activeTransportProfile.store(
        transportProfile,
        std::memory_order_relaxed
    );
    receiver->activePcmFormat.store(pcmFormat, std::memory_order_relaxed);
    receiver->expectedStreamConfiguration.protocolVersion =
        AOO_LOW_LATENCY_PROTOCOL_VERSION;
    receiver->expectedStreamConfiguration.profile =
        static_cast<AooByte>(transportProfile);
    receiver->expectedStreamConfiguration.pcmFormat = transportProfile
            == AOOAppleTransportProfileAutomatic
        ? 0
        : static_cast<AooByte>(pcmFormat);
    receiver->expectedStreamConfiguration.channelCount =
        static_cast<AooUInt16>(sourceChannelCount);
    receiver->expectedStreamConfiguration.blockFrames =
        static_cast<AooUInt16>(receiver->streamBlockSize);
    receiver->expectedStreamConfiguration.datagramBytes =
        static_cast<AooUInt16>(receiver->packetSize);
    receiver->expectedStreamConfiguration.channelMapCount =
        static_cast<AooUInt16>(sourceChannelCount);
    receiver->expectedStreamConfiguration.sampleRate =
        static_cast<AooUInt32>(receiver->sampleRate);
    receiver->expectedStreamConfiguration.targetLatencyFrames =
        static_cast<AooUInt32>(std::max(
            receiver->streamBlockSize,
            static_cast<int32_t>(std::ceil(
                clampedLatencySeconds(latencyMilliseconds) * receiver->sampleRate
            ))
        ));
    receiver->expectedStreamConfiguration.capabilities = capabilities;
    for (int32_t channel = 0; channel < sourceChannelCount; ++channel) {
        receiver->expectedStreamConfiguration.channelMap[channel] =
            static_cast<AooByte>(sourceChannelIndices[channel]);
    }
    receiver->sourceChannelStorage.assign(
        static_cast<size_t>(receiver->channelCount * receiver->maximumBlockSize),
        0
    );
    receiver->channelStorage.assign(
        static_cast<size_t>(receiver->channelCount * receiver->maximumBlockSize),
        0
    );
    receiver->channelPeaks = std::make_unique<std::atomic<float>[]>(
        static_cast<size_t>(receiver->channelCount)
    );
    for (int32_t channel = 0; channel < receiver->channelCount; ++channel) {
        receiver->sourceChannelPointers[channel] = receiver->sourceChannelStorage.data()
            + channel * receiver->maximumBlockSize;
        receiver->channelPointers[channel] = receiver->channelStorage.data()
            + channel * receiver->maximumBlockSize;
        receiver->sourceChannelMap[channel].store(channel, std::memory_order_relaxed);
        receiver->channelPeaks[channel].store(0, std::memory_order_relaxed);
    }

    receiver->sink = AooSink_new(sinkID);
    receiver->client = AooClient_new();
    if (!receiver->sink || !receiver->client) {
        if (errorCode) {
            *errorCode = kAooErrorOutOfMemory;
        }
        return nullptr;
    }

    AooError result = AooSink_setup(
        receiver->sink,
        receiver->channelCount,
        receiver->sampleRate,
        receiver->maximumBlockSize,
        receiver->fixedBlockSizeEnabled ? kAooFixedBlockSize : 0
    );
    if (result == kAooOk && receiver->fixedBlockSizeEnabled) {
        result = AooSink_setDynamicResampling(receiver->sink, kAooTrue);
    }
    const double targetLatencySeconds = clampedLatencySeconds(latencyMilliseconds);
    if (result == kAooOk) {
        result = AooSink_setLatency(
            receiver->sink,
            targetLatencySeconds
        );
        if (result == kAooOk) {
            receiver->targetLatencySeconds.store(
                targetLatencySeconds,
                std::memory_order_release
            );
        }
    }
    if (result == kAooOk
        && receiver->transportProfile != AOOAppleTransportProfileAutomatic) {
        result = AooSink_setLowLatencyTarget(
            receiver->sink,
            static_cast<AooUInt32>(std::llround(
                targetLatencySeconds * receiver->sampleRate
            ))
        );
    }
    if (result == kAooOk) {
        // Capacity absorbs early Wi-Fi bursts; AOO's latency setting remains
        // the independent playout target and therefore controls audible delay.
        result = AooSink_setBufferSize(
            receiver->sink,
            receiverBufferCapacitySeconds(
                targetLatencySeconds,
                receiver->transportProfile,
                receiver->streamBlockSize,
                receiver->sampleRate
            )
        );
    }
    if (result == kAooOk) {
        result = AooSink_setPacketSize(receiver->sink, receiver->packetSize);
    }
    if (result == kAooOk) {
        result = AooSink_setPingInterval(receiver->sink, kPingIntervalSeconds);
    }
    if (result == kAooOk) {
        result = AooSink_setResendData(
            receiver->sink,
            receiver->transportProfile == AOOAppleTransportProfileAdaptiveWireless
                || receiver->transportProfile == AOOAppleTransportProfileAutomatic
                ? kAooTrue
                : kAooFalse
        );
    }
    if (result == kAooOk
        && (receiver->transportProfile == AOOAppleTransportProfileAdaptiveWireless
            || receiver->transportProfile == AOOAppleTransportProfileAutomatic)) {
        result = AooSink_setResendInterval(
            receiver->sink,
            static_cast<double>(receiver->streamBlockSize) / receiver->sampleRate
        );
    }
    if (result == kAooOk
        && (receiver->transportProfile == AOOAppleTransportProfileAdaptiveWireless
            || receiver->transportProfile == AOOAppleTransportProfileAutomatic)) {
        result = AooSink_setResendLimit(receiver->sink, 1);
    }
    if (result == kAooOk) {
        result = AooSink_setEventHandler(
            receiver->sink,
            handleReceiverEvent,
            receiver.get(),
            kAooEventModePoll
        );
    }
    if (result != kAooOk) {
        if (errorCode) {
            *errorCode = result;
        }
        return nullptr;
    }

    AooClientSettings settings;
    settings.portNumber = static_cast<AooUInt16>(receiver->localPort);
    result = AooClient_setup(receiver->client, &settings);
    if (result == kAooOk) {
        receiver->localPort = settings.portNumber;
        result = AooClient_addSink(receiver->client, receiver->sink);
    }
    if (result != kAooOk) {
        if (errorCode) {
            *errorCode = result;
        }
        return nullptr;
    }

    receiver->metrics.ready.store(1, std::memory_order_release);
    startReceiverNetworkThreads(receiver.get());
    startReceiverEventThread(receiver.get());
    startReceiverAdaptiveThread(receiver.get());
    return receiver.release();
}

void AOOAppleReceiverDestroy(AOOAppleReceiver *receiver) {
    delete receiver;
}

int32_t AOOAppleReceiverSetLatency(
    AOOAppleReceiver *receiver,
    double latencyMilliseconds
) {
    if (!receiver || !receiver->sink) {
        return kAooErrorBadArgument;
    }
    if (receiver->transportProfile == AOOAppleTransportProfileDeterministicWired
        || (receiver->transportProfile == AOOAppleTransportProfileAutomatic
            && receiver->activeTransportProfile.load(std::memory_order_acquire)
                == AOOAppleTransportProfileDeterministicWired)) {
        return kAooErrorNotPermitted;
    }
    const double targetLatencySeconds = clampedLatencySeconds(latencyMilliseconds);
    if (receiver->targetLatencySeconds.load(std::memory_order_acquire)
        == targetLatencySeconds) {
        receiver->metrics.lastError.store(kAooOk, std::memory_order_relaxed);
        return kAooOk;
    }
    AooError result = AooSink_setLowLatencyTarget(
        receiver->sink,
        static_cast<AooUInt32>(std::llround(
            targetLatencySeconds * receiver->sampleRate
        ))
    );
    if (result == kAooOk) {
        receiver->targetLatencySeconds.store(
            targetLatencySeconds,
            std::memory_order_release
        );
    }
    receiver->metrics.lastError.store(result, std::memory_order_relaxed);
    return result;
}

void AOOAppleReceiverSetOutputPresentationLatency(
    AOOAppleReceiver *receiver,
    double latencyMilliseconds
) {
    if (!receiver) {
        return;
    }
    const double resolvedLatency = std::isfinite(latencyMilliseconds)
        ? std::max(0.0, latencyMilliseconds)
        : 0.0;
    receiver->metrics.outputPresentationLatencyMilliseconds.store(
        resolvedLatency,
        std::memory_order_relaxed
    );
}

void AOOAppleReceiverSetMonitorPair(
    AOOAppleReceiver *receiver,
    int32_t monitorPairStartChannel
) {
    if (!receiver) {
        return;
    }
    if (monitorPairStartChannel <= 0) {
        receiver->monitorPairStartChannel.store(0, std::memory_order_release);
        return;
    }
    const int32_t pair = std::clamp(
        monitorPairStartChannel,
        1,
        std::max(1, receiver->channelCount - 1)
    );
    receiver->monitorPairStartChannel.store(
        (pair & 1) == 0 ? pair - 1 : pair,
        std::memory_order_release
    );
}

int32_t AOOAppleReceiverProcessStereoAtTime(
    AOOAppleReceiver *receiver,
    float *outputLeft,
    float *outputRight,
    int32_t frameCount,
    uint64_t sinkNtpTime
) {
    if (!receiver || !outputLeft || !outputRight || frameCount <= 0
        || sinkNtpTime == 0) {
        return kAooErrorBadArgument;
    }
    if (receiver->fixedBlockSizeEnabled && frameCount != receiver->maximumBlockSize) {
        const size_t byteCount = static_cast<size_t>(frameCount) * sizeof(float);
        std::memset(outputLeft, 0, byteCount);
        std::memset(outputRight, 0, byteCount);
        receiver->metrics.processCalls.fetch_add(1, std::memory_order_relaxed);
        receiver->metrics.processedFrames.fetch_add(
            static_cast<uint64_t>(frameCount),
            std::memory_order_relaxed
        );
        receiver->metrics.processErrors.fetch_add(1, std::memory_order_relaxed);
        receiver->metrics.processBlockMismatches.fetch_add(1, std::memory_order_relaxed);
        receiver->metrics.lastError.store(kAooErrorBadArgument, std::memory_order_relaxed);
        receiver->metrics.lastProcessFrameCount.store(frameCount, std::memory_order_relaxed);
        return kAooErrorBadArgument;
    }
    const int32_t pairStart = receiver->monitorPairStartChannel.load(
        std::memory_order_acquire
    );
    const bool monitoringEnabled = pairStart > 0;
    const int32_t leftChannel = monitoringEnabled
        ? std::clamp(pairStart - 1, 0, receiver->channelCount - 1)
        : 0;
    const int32_t rightChannel = monitoringEnabled
        ? std::min(leftChannel + 1, receiver->channelCount - 1)
        : 0;
    int32_t processedFrames = 0;
    AooError finalResult = kAooOk;

    while (processedFrames < frameCount) {
        const int32_t chunkFrames = std::min(
            receiver->maximumBlockSize,
            frameCount - processedFrames
        );
        receiver->currentSinkProcessNtpTime = AOOAppleNTPTimeOffsetFrames(
            sinkNtpTime,
            processedFrames,
            receiver->sampleRate
        );
        const AooError result = AooSink_process(
            receiver->sink,
            receiver->sourceChannelPointers.data(),
            chunkFrames,
            receiver->currentSinkProcessNtpTime,
            handleReceiverStreamMessage,
            receiver
        );
        if (result != kAooOk && result != kAooErrorIdle && result != kAooErrorWouldBlock) {
            finalResult = result;
            receiver->metrics.processErrors.fetch_add(1, std::memory_order_relaxed);
            receiver->metrics.lastError.store(result, std::memory_order_relaxed);
        }

        const size_t chunkByteCount = static_cast<size_t>(chunkFrames) * sizeof(float);
        for (int32_t channel = 0; channel < receiver->channelCount; ++channel) {
            std::memset(receiver->channelPointers[channel], 0, chunkByteCount);
        }
        std::array<int32_t, kMaximumChannelCount> channelMap{};
        const uint64_t mapRevisionBefore = receiver->sourceChannelMapRevision.load(
            std::memory_order_acquire
        );
        int32_t channelMapCount = 0;
        if ((mapRevisionBefore & 1) == 0) {
            channelMapCount = receiver->sourceChannelMapCount.load(
                std::memory_order_relaxed
            );
            for (int32_t channel = 0; channel < channelMapCount; ++channel) {
                channelMap[channel] = receiver->sourceChannelMap[channel].load(
                    std::memory_order_relaxed
                );
            }
        }
        const uint64_t mapRevisionAfter = receiver->sourceChannelMapRevision.load(
            std::memory_order_acquire
        );
        const bool mapSnapshotIsStable = mapRevisionBefore == mapRevisionAfter
            && (mapRevisionAfter & 1) == 0;
        const bool hasChannelMap = mapSnapshotIsStable && channelMapCount > 0;
        const int32_t sourceChannelCount = !mapSnapshotIsStable
            ? 0
            : hasChannelMap
                ? channelMapCount
                : std::clamp(
                    receiver->metrics.sourceChannelCount.load(std::memory_order_relaxed),
                    0,
                    receiver->channelCount
                );
        for (int32_t sourceChannel = 0;
             sourceChannel < sourceChannelCount;
             ++sourceChannel) {
            const int32_t destinationChannel = hasChannelMap
                ? channelMap[sourceChannel]
                : sourceChannel;
            if (destinationChannel >= 0 && destinationChannel < receiver->channelCount) {
                AooSample *destination = receiver->channelPointers[destinationChannel];
                const AooSample *source = receiver->sourceChannelPointers[sourceChannel];
                for (int32_t frame = 0; frame < chunkFrames; ++frame) {
                    destination[frame] = finiteSample(source[frame]);
                }
            }
        }

        if (monitoringEnabled) {
            std::memcpy(
                outputLeft + processedFrames,
                receiver->channelPointers[leftChannel],
                chunkByteCount
            );
            std::memcpy(
                outputRight + processedFrames,
                receiver->channelPointers[rightChannel],
                chunkByteCount
            );
        } else {
            std::memset(outputLeft + processedFrames, 0, chunkByteCount);
            std::memset(outputRight + processedFrames, 0, chunkByteCount);
        }
        for (int32_t channel = 0; channel < receiver->channelCount; ++channel) {
            float peak = 0;
            const float *samples = receiver->channelPointers[channel];
            for (int32_t frame = 0; frame < chunkFrames; ++frame) {
                peak = std::max(peak, std::abs(samples[frame]));
            }
            accumulatePeak(receiver->channelPeaks[channel], peak);
        }
        processedFrames += chunkFrames;
    }

    receiver->metrics.processCalls.fetch_add(1, std::memory_order_relaxed);
    receiver->metrics.processedFrames.fetch_add(
        static_cast<uint64_t>(frameCount),
        std::memory_order_relaxed
    );
    receiver->metrics.lastProcessFrameCount.store(frameCount, std::memory_order_relaxed);
    int32_t minimumFrameCount = receiver->metrics.minimumProcessFrameCount.load(
        std::memory_order_relaxed
    );
    while (frameCount < minimumFrameCount
           && !receiver->metrics.minimumProcessFrameCount.compare_exchange_weak(
               minimumFrameCount,
               frameCount,
               std::memory_order_relaxed
           )) {}
    int32_t maximumFrameCount = receiver->metrics.maximumProcessFrameCount.load(
        std::memory_order_relaxed
    );
    while (frameCount > maximumFrameCount
           && !receiver->metrics.maximumProcessFrameCount.compare_exchange_weak(
               maximumFrameCount,
               frameCount,
               std::memory_order_relaxed
           )) {}
    AooClient_notify(receiver->client);
    return finalResult;
}

void AOOAppleReceiverGetStatus(
    AOOAppleReceiver *receiver,
    AOOAppleReceiverStatus *status
) {
    if (!status) {
        return;
    }
    std::memset(status, 0, sizeof(*status));
    status->bufferFillRatio = -1;
    status->bufferedAudioMilliseconds = -1;
    if (!receiver) {
        return;
    }
    status->isReady = receiver->metrics.ready.load(std::memory_order_acquire);
    status->streamActive = receiver->metrics.streamActive.load(std::memory_order_acquire);
    status->streamState = receiver->metrics.streamState.load(std::memory_order_acquire);
    status->localPort = receiver->localPort;
    status->channelCount = receiver->channelCount;
    status->sampleRate = receiver->sampleRate;
    status->maximumBlockSize = receiver->maximumBlockSize;
    status->fixedBlockSizeEnabled = receiver->fixedBlockSizeEnabled ? 1 : 0;
    AooBool dynamicResamplingEnabled = kAooFalse;
    if (AooSink_getDynamicResampling(receiver->sink, &dynamicResamplingEnabled) == kAooOk) {
        status->dynamicResamplingEnabled = dynamicResamplingEnabled != 0 ? 1 : 0;
    }
    status->lastProcessFrameCount = receiver->metrics.lastProcessFrameCount.load(std::memory_order_relaxed);
    const int32_t minimumProcessFrameCount = receiver->metrics.minimumProcessFrameCount.load(std::memory_order_relaxed);
    status->minimumProcessFrameCount = minimumProcessFrameCount == INT32_MAX ? 0 : minimumProcessFrameCount;
    status->maximumProcessFrameCount = receiver->metrics.maximumProcessFrameCount.load(std::memory_order_relaxed);
    status->sourceChannelCount = receiver->metrics.sourceChannelCount.load(std::memory_order_relaxed);
    status->sourceSampleRate = receiver->metrics.sourceSampleRate.load(std::memory_order_relaxed);
    status->sourceBlockSize = receiver->metrics.sourceBlockSize.load(std::memory_order_relaxed);
    status->transportProfile = receiver->activeTransportProfile.load(
        std::memory_order_acquire
    );
    status->pcmFormat = receiver->activePcmFormat.load(
        std::memory_order_acquire
    );
    status->lastError = receiver->metrics.lastError.load(std::memory_order_relaxed);
    status->processCallCount = receiver->metrics.processCalls.load(std::memory_order_relaxed);
    status->processedFrameCount = receiver->metrics.processedFrames.load(std::memory_order_relaxed);
    status->processErrorCount = receiver->metrics.processErrors.load(std::memory_order_relaxed);
    status->processBlockMismatchCount = receiver->metrics.processBlockMismatches.load(std::memory_order_relaxed);
    status->streamStartCount = receiver->metrics.streamStarts.load(std::memory_order_relaxed);
    status->streamActiveCount = receiver->metrics.streamActiveTransitions.load(std::memory_order_relaxed);
    status->streamBufferingCount = receiver->metrics.streamBufferingTransitions.load(std::memory_order_relaxed);
    status->streamInactiveCount = receiver->metrics.streamInactiveTransitions.load(std::memory_order_relaxed);
    status->bufferUnderrunCount = receiver->metrics.bufferUnderruns.load(std::memory_order_relaxed);
    status->bufferOverrunCount = receiver->metrics.bufferOverruns.load(std::memory_order_relaxed);
    status->droppedBlockCount = receiver->metrics.droppedBlocks.load(std::memory_order_relaxed);
    status->resentBlockCount = receiver->metrics.resentBlocks.load(std::memory_order_relaxed);
    status->sourceXRunCount = receiver->metrics.sourceXRuns.load(std::memory_order_relaxed);
    status->concealmentCount = receiver->metrics.concealments.load(std::memory_order_relaxed);
    status->reacquisitionCount = receiver->metrics.reacquisitions.load(std::memory_order_relaxed);
    status->incompatibleStreamCount = receiver->metrics.incompatibleStreams.load(
        std::memory_order_relaxed
    );
    status->adaptiveAdjustmentCount = receiver->metrics.adaptiveAdjustments.load(
        std::memory_order_relaxed
    );
    status->pingEventCount = receiver->metrics.pingEvents.load(std::memory_order_acquire);
    status->aooStreamTimeEventCount = receiver->metrics.aooStreamTimeEvents.load(
        std::memory_order_acquire
    );
    status->streamTimeEventCount = receiver->metrics.streamTimeEvents.load(std::memory_order_acquire);
    status->presentationTimestampEventCount = receiver->metrics.presentationTimestampEvents.load(
        std::memory_order_acquire
    );
    status->sourceSamplePosition = receiver->metrics.sourceSamplePosition.load(
        std::memory_order_relaxed
    );
    status->sourceLatencyMilliseconds = receiver->metrics.sourceLatencyMilliseconds.load(std::memory_order_relaxed);
    status->sinkLatencyMilliseconds = receiver->metrics.sinkLatencyMilliseconds.load(std::memory_order_relaxed);
    status->jitterBufferLatencyMilliseconds = receiver->metrics.jitterBufferLatencyMilliseconds.load(std::memory_order_relaxed);
    status->rawRoundTripMilliseconds = receiver->metrics.rawRoundTripMilliseconds.load(std::memory_order_relaxed);
    status->rawClockOffsetMilliseconds = receiver->metrics.rawClockOffsetMilliseconds.load(std::memory_order_relaxed);
    status->rawAOOStreamTimeDeltaMilliseconds =
        receiver->metrics.rawAOOStreamTimeDeltaMilliseconds.load(
            std::memory_order_relaxed
        );
    status->rawStreamTimeDeltaMilliseconds = receiver->metrics.rawStreamTimeDeltaMilliseconds.load(std::memory_order_relaxed);
    status->rawPresentationToSinkCallbackDeltaMilliseconds =
        receiver->metrics.rawPresentationToSinkCallbackDeltaMilliseconds.load(
            std::memory_order_relaxed
        );
    status->outputPresentationLatencyMilliseconds = receiver->metrics.outputPresentationLatencyMilliseconds.load(std::memory_order_relaxed);
    AooLowLatencySinkStatistics lowLatencyStatistics{};
    if (AooSink_getLowLatencyStatistics(
            receiver->sink,
            &lowLatencyStatistics
        ) == kAooOk) {
        status->arrivalObservationCount =
            lowLatencyStatistics.arrivalObservationCount;
        status->latestArrivalResidualMilliseconds =
            lowLatencyStatistics.latestArrivalResidual * 1000.0;
        status->p99ArrivalResidualMilliseconds =
            lowLatencyStatistics.p99ArrivalResidual * 1000.0;
    }
    status->targetLatencyMilliseconds = receiver->targetLatencySeconds.load(
        std::memory_order_acquire
    ) * 1000.0;
    AooSeconds bufferCapacitySeconds = 0;
    if (AooSink_getBufferSize(receiver->sink, &bufferCapacitySeconds) == kAooOk
        && std::isfinite(bufferCapacitySeconds)) {
        const double effectiveCapacitySeconds = bufferCapacitySeconds > 0
            ? bufferCapacitySeconds
            : receiver->targetLatencySeconds.load(std::memory_order_acquire) * 2.0;
        status->bufferCapacityMilliseconds = effectiveCapacitySeconds * 1000.0;
    }
    AooSampleRate realSampleRate = 0;
    if (AooSink_getRealSampleRate(receiver->sink, &realSampleRate) == kAooOk
        && std::isfinite(realSampleRate)) {
        status->realSampleRate = realSampleRate;
    }
    if (status->sourceChannelCount > 0) {
        status->bufferFillRatio = receiverBufferFillRatio(receiver);
        if (status->bufferFillRatio >= 0 && status->bufferCapacityMilliseconds > 0) {
            status->bufferedAudioMilliseconds = status->bufferFillRatio
                * status->bufferCapacityMilliseconds;
        }
    }
}

int32_t AOOAppleReceiverCopyChannelPeaks(
    const AOOAppleReceiver *receiver,
    float *destination,
    int32_t capacity
) {
    if (!receiver || !destination || capacity <= 0) {
        return 0;
    }
    const int32_t count = std::min(capacity, receiver->channelCount);
    for (int32_t channel = 0; channel < count; ++channel) {
        destination[channel] = receiver->channelPeaks[channel].exchange(
            0,
            std::memory_order_relaxed
        );
    }
    return count;
}
