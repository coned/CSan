// SPDX-License-Identifier: MIT
// A relaxed RMW continues the release sequence to a later acquire.
// Expected: no race.
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
        while (flag->load(std::memory_order_relaxed) != 1) {
        }
        flag->fetch_add(1, std::memory_order_relaxed); // continues the sequence
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
    middle.join();
    reader.join();
    return csan_test_finish(false);
}
