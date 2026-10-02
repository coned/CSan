// SPDX-License-Identifier: MIT
// As the CAS-failure handoff, but the failure ordering is relaxed and acquires nothing.
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
        int expected = 0;
        while (flag->compare_exchange_strong(expected, 0, std::memory_order_release,
                                             std::memory_order_relaxed)) {
            expected = 0;
        }
        volatile char v = data[0]; // nothing acquired: unordered
        (void)v;
    });
    data[0] = 42;
    flag->store(1, std::memory_order_release);
    consumer.join();
    return csan_test_finish(true);
}
