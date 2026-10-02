// SPDX-License-Identifier: MIT
// A failing compare-exchange with acquire failure ordering is the only edge for the data read.
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
        int expected = 0;
        while (flag->compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
            expected = 0;
        }
        // The loop left on a failure that saw flag == 1: acquired.
        volatile char v = data[0];
        (void)v;
    });
    data[0] = 42;
    flag->store(1, std::memory_order_release);
    consumer.join();
    return csan_test_finish(false);
}
