// SPDX-License-Identifier: MIT
// A reads x, releases a flag, and reads x again; B acquires the flag and
// writes x. The release covers A's first read only, so the second read,
// which a load without the line lock must not take as a repeat, races.
// Expected: race (write after read), exactly one.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    auto* x = static_cast<uint64_t*>(csan_alloc(8));
    *x = 1;
    std::atomic<int> flag{0};
    std::atomic<bool> second{false};
    std::thread a([&] {
        volatile uint64_t v = *x;
        v = v + *x; // a repeat at the same clock: no change
        flag.store(1, std::memory_order_release);
        v = v + *x; // a read at a later clock
        second.store(true, std::memory_order_relaxed);
        (void)v;
    });
    std::thread b([&] {
        while (flag.load(std::memory_order_acquire) == 0) {
        }
        while (!second.load(std::memory_order_relaxed)) {
        }
        *x = 2;
    });
    a.join();
    b.join();
    return csan_expect_race(CSAN_RACE_DATA_RACE_RW).races(1).finish();
}
