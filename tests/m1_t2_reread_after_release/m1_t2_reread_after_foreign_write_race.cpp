// SPDX-License-Identifier: MIT
// A reads x twice, B writes x with no edge, A reads x again: the third read
// sees a new W_x and must not pass as a repeat of the first two.
// Expected: race: B's write after A's reads, and A's read after B's write.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdlib>
#include <thread>

int main() {
    setenv("CSAN_REPORT_ONCE", "0", 1); // B's finding marks x; A's read must be checked still
    csan_init(1, 0, 1u << 16);
    auto* x = static_cast<uint64_t*>(csan_alloc(8));
    *x = 1;
    std::atomic<int> step{0};
    std::thread a([&] {
        volatile uint64_t v = *x;
        v = v + *x;
        step.store(1, std::memory_order_relaxed);
        while (step.load(std::memory_order_relaxed) != 2) {
        }
        v = v + *x;
        (void)v;
    });
    std::thread b([&] {
        while (step.load(std::memory_order_relaxed) != 1) {
        }
        *x = 2;
        step.store(2, std::memory_order_relaxed);
    });
    a.join();
    b.join();
    return csan_expect_race(CSAN_RACE_DATA_RACE_RW).kind(CSAN_RACE_DATA_RACE_WR).races(2).finish();
}
