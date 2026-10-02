// SPDX-License-Identifier: MIT
// Two threads do unsynchronized relaxed RMWs on one counter; atomics never race.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    auto* counter = new (csan_alloc(sizeof(std::atomic<long>))) std::atomic<long>(0);

    std::thread other([&] {
        for (int i = 0; i < 100; ++i) {
            counter->fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int i = 0; i < 100; ++i) {
        counter->fetch_add(1, std::memory_order_relaxed);
    }
    other.join();
    return csan_test_finish(false);
}
