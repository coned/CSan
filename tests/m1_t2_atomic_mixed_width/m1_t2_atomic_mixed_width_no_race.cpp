// SPDX-License-Identifier: MIT
// Unsynchronized relaxed RMWs on a 64-bit atomic and on its low 32 bits; atomics never race.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdint>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    void* raw = csan_alloc(sizeof(std::atomic<uint64_t>));
    auto* wide = new (raw) std::atomic<uint64_t>(0);
    auto* narrow = reinterpret_cast<std::atomic<uint32_t>*>(raw); // low 4 bytes of the same word

    std::thread other([&] {
        for (int i = 0; i < 100; ++i) {
            narrow->fetch_add(1, std::memory_order_relaxed); // splits the word
        }
    });
    for (int i = 0; i < 100; ++i) {
        wide->fetch_add(1, std::memory_order_relaxed);
    }
    other.join();
    return csan_test_finish(false);
}
