// SPDX-License-Identifier: MIT
// Concurrent reads fill the arena with read lists; a write frees one; two
// concurrent reads of y then need a block again, and get the freed one.
// Then a write ordered after only the first of them.
// Expected: race (write after read): the second read is recorded, so an
// exhausted arena is not taken as exhausted for good.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <thread>

namespace {
constexpr uint32_t kWords = 40000; // more read lists than a 1 MiB arena holds
}

int main() {
    setenv("CSAN_ARENA", "1M", 1);
    csan_init(1, 0, 1u << 20);
    auto* w = static_cast<uint64_t*>(csan_alloc(kWords * sizeof(uint64_t)));
    auto* y = static_cast<uint64_t*>(csan_alloc(sizeof(uint64_t)));
    for (uint32_t i = 0; i < kWords; ++i) {
        w[i] = i;
    }
    *y = 1;

    // A then B read every word, with no edge between them: each word's R_x
    // becomes a read list, until the arena runs out.
    std::atomic<bool> a_done{false};
    std::thread a([&] {
        uint64_t s = 0;
        for (uint32_t i = 0; i < kWords; ++i) {
            s += w[i];
        }
        a_done.store(s != 0, std::memory_order_relaxed);
    });
    std::thread b([&] {
        while (!a_done.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        volatile uint64_t s = 0;
        for (uint32_t i = 0; i < kWords; ++i) {
            s = s + w[i];
        }
    });
    a.join();
    b.join();
    w[0] = 7; // after both readers: frees w[0]'s read list

    std::atomic<bool> c_done{false};
    std::atomic<bool> d_done{false};
    std::thread c([&] {
        volatile uint64_t v = *y;
        (void)v;
        c_done.store(true, std::memory_order_relaxed);
    });
    std::thread d([&] {
        while (!c_done.load(std::memory_order_relaxed)) {
            std::this_thread::yield();
        }
        volatile uint64_t v = *y; // concurrent with c's read: needs a read list
        (void)v;
        d_done.store(true, std::memory_order_relaxed);
    });
    c.join();
    while (!d_done.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    *y = 2; // after c's read, concurrent with d's
    d.join();
    return csan_expect_race(CSAN_RACE_DATA_RACE_RW).races(1).finish();
}
