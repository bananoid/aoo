#include "aoo.h"
#include "aoo/src/data_frame.hpp"
#include "common/lockfree.hpp"

#include <array>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

int main() {
    CHECK(aoo_initialize(nullptr) == kAooOk);

    {
        aoo::data_frame_allocator allocator;
        CHECK(allocator.reserve(1200, 8) == 8);
        CHECK(allocator.reserve(1200, 4) == 0);
        CHECK(allocator.reserve(1200, 8) == 0);

        std::array<aoo::data_frame *, 8> frames{};
        for (auto& frame : frames) {
            frame = allocator.allocate(1200);
            CHECK(frame != nullptr);
        }
        for (auto frame : frames) {
            allocator.deallocate(frame);
        }
        CHECK(allocator.reserve(1200, 8) == 0);
    }

    {
        aoo::lockfree::unbounded_mpsc_queue<int> queue;
        queue.reserve(16);
        queue.reserve(4);
        queue.reserve(16);
        for (int value = 0; value < 16; ++value) {
            queue.push(value);
        }
        for (int expected = 0; expected < 16; ++expected) {
            int value = -1;
            CHECK(queue.try_pop(value));
            CHECK(value == expected);
        }
        CHECK(queue.empty());
        queue.reserve(8);
    }

    aoo_terminate();
    return 0;
}
