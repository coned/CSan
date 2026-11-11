// SPDX-License-Identifier: MIT
// Release/acquire through an atomic byte in a word that is split (its other
// bytes written plainly), and through a fresh word: an acquire load must find
// S_x in both, whatever shortcut skips words that carry no S_x.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    auto* w = static_cast<unsigned char*>(csan_alloc(64));
    auto* data = static_cast<uint64_t*>(csan_alloc(16));
    w[0] = 1; // byte writes split the word holding the flag
    w[1] = 1;
    auto* flag = reinterpret_cast<std::atomic<unsigned char>*>(&w[3]);
    auto* flag2 = reinterpret_cast<std::atomic<uint64_t>*>(&w[8]);
    flag->store(0, std::memory_order_relaxed);
    flag2->store(0, std::memory_order_relaxed);
    std::thread a([&] {
        data[0] = 42;
        flag->store(1, std::memory_order_release);
        data[1] = 43;
        flag2->store(1, std::memory_order_release);
    });
    std::thread b([&] {
        while (flag->load(std::memory_order_acquire) == 0) {
        }
        volatile uint64_t v = data[0];
        while (flag2->load(std::memory_order_acquire) == 0) {
        }
        v = v + data[1];
        (void)v;
    });
    a.join();
    b.join();
    return csan_test_finish(false);
}
