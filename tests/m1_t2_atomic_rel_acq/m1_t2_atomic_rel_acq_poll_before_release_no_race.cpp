// SPDX-License-Identifier: MIT
// B polls a flag in ordinary (non-pool) memory long before A's release store
// creates its S_x: the acquire that finally sees the value must join it, not
// reuse the earlier finding that the flag had no clock.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    auto* data = static_cast<uint64_t*>(csan_alloc(8));
    std::atomic<int> flag{0};
    std::atomic<int> polls{0};
    std::thread b([&] {
        while (flag.load(std::memory_order_acquire) == 0) {
            polls.fetch_add(1, std::memory_order_relaxed);
        }
        volatile uint64_t v = *data;
        (void)v;
    });
    while (polls.load(std::memory_order_relaxed) < 1000) {
    }
    std::thread a([&] {
        *data = 42;
        flag.store(1, std::memory_order_release);
    });
    a.join();
    b.join();
    return csan_test_finish(false);
}
