// SPDX-License-Identifier: MIT
// A mutex in hardware-coherent memory still orders the software-coherent data it protects.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <mutex>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);

    // The data: ordinary software-coherent pool memory, checked as always.
    auto* data = static_cast<volatile uint64_t*>(csan_alloc(8));
    *data = 0;

    // The lock: pool memory the program declares hardware-coherent.
    void* lock_mem = csan_alloc(sizeof(std::mutex));
    csan_register_hwcc_range(lock_mem, sizeof(std::mutex));
    auto* m = new (lock_mem) std::mutex();

    std::thread writer([&] {
        std::lock_guard<std::mutex> g(*m);
        *data = 42;
    });
    std::thread reader([&] {
        std::lock_guard<std::mutex> g(*m);
        volatile uint64_t v = *data;
        (void)v;
    });
    writer.join();
    reader.join();

    return csan_expect_no_race().finish();
}
