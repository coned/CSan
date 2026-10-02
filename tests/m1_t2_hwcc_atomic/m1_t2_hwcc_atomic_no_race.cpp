// SPDX-License-Identifier: MIT
// A release/acquire flag in hardware-coherent memory still orders software-coherent data.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);

    // The data: ordinary software-coherent pool memory, checked as always.
    auto* data = static_cast<volatile uint64_t*>(csan_alloc(8));
    *data = 0;

    // The flag: pool memory the program declares hardware-coherent.
    void* flag_mem = csan_alloc(sizeof(std::atomic<int>));
    csan_register_hwcc_range(flag_mem, sizeof(std::atomic<int>));
    auto* flag = new (flag_mem) std::atomic<int>(0);

    std::thread consumer([&] {
        while (flag->load(std::memory_order_acquire) != 1) {
        }
        volatile uint64_t v = *data; // ordered after the write by the acquire
        (void)v;
    });
    *data = 42;
    flag->store(1, std::memory_order_release);
    consumer.join();

    return csan_expect_no_race().finish();
}
