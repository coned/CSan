// SPDX-License-Identifier: MIT
// Release fence + relaxed store, relaxed load + acquire fence.
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
        while (flag->load(std::memory_order_relaxed) != 1) {
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        volatile char v = data[0];
        (void)v;
    });
    data[0] = 42;
    std::atomic_thread_fence(std::memory_order_release);
    flag->store(1, std::memory_order_relaxed);
    consumer.join();
    return csan_test_finish(false);
}
