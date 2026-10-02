// SPDX-License-Identifier: MIT
// Data handed off by a release store and an acquire load of a flag.
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

    std::thread consumer([&] {
        while (flag->load(std::memory_order_acquire) != 1) {
        }
        volatile char v = data[0]; // ordered after the write by the acquire
        (void)v;
    });
    data[0] = 42;
    flag->store(1, std::memory_order_release);
    consumer.join();
    return csan_test_finish(false);
}
