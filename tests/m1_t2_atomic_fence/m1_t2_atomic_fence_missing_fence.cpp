// SPDX-License-Identifier: MIT
// Relaxed flag handoff with no fences: the data accesses are unordered.
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

    std::thread consumer([&] {
        while (flag->load(std::memory_order_relaxed) != 1) {
        }
        volatile char v = data[0]; // no acquire fence: unordered
        (void)v;
    });
    data[0] = 42;
    flag->store(1, std::memory_order_relaxed); // no release fence: publishes nothing
    consumer.join();
    return csan_test_finish(true);
}
