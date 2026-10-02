// SPDX-License-Identifier: MIT
// The releasing thread's own relaxed store ends its release sequence (C++20 rule).
// Expected: race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* data = static_cast<char*>(csan_alloc(8));
    auto* flag = new (csan_alloc(sizeof(std::atomic<int>))) std::atomic<int>(0);
    auto* ready = new (csan_alloc(sizeof(std::atomic<int>))) std::atomic<int>(0);

    std::thread middle([&] {
        while (flag->load(std::memory_order_relaxed) != 2) {
        }
        ready->store(1, std::memory_order_relaxed);
    });
    std::thread reader([&] {
        while (ready->load(std::memory_order_relaxed) != 1) {
        }
        (void)flag->load(std::memory_order_acquire); // the one acquire
        volatile char v = data[0];
        (void)v;
    });
    data[0] = 42;
    flag->store(1, std::memory_order_release); // heads the sequence
    flag->store(2, std::memory_order_relaxed); // same thread: ENDS it anyway
    middle.join();
    reader.join();
    return csan_expect_race(CSAN_RACE_READ_BEFORE_HB_WRITE).races(1).finish();
}
