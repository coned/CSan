// SPDX-License-Identifier: MIT
// One thread does relaxed RMWs on a counter while another writes its bytes plainly.
// Expected: race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    void* raw = csan_alloc(sizeof(std::atomic<long>));
    auto* counter = new (raw) std::atomic<long>(0);
    auto* bytes = static_cast<volatile char*>(raw);

    std::thread other([&] {
        for (int i = 0; i < 100; ++i) {
            counter->fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int i = 0; i < 100; ++i) {
        bytes[0] = static_cast<char>(i); // plain write to an atomic's storage
    }
    other.join();
    return csan_test_finish(true);
}
