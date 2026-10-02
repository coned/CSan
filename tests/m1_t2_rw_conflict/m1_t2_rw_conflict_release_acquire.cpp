// SPDX-License-Identifier: MIT
// x itself is accessed by a release store and an acquire load.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    auto* x = new (csan_alloc(sizeof(std::atomic<char>))) std::atomic<char>{0};
    volatile int go = 0;

    std::thread writer([&] {
        x->store(42, std::memory_order_release); // release write x
        go = 1;
    });
    std::thread reader([&] {
        while (!go) {
            std::this_thread::yield();
        }
        volatile char v = x->load(std::memory_order_acquire); // acquire read x
        (void)v;
    });
    writer.join();
    reader.join();
    return csan_test_finish(false);
}
