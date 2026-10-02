// SPDX-License-Identifier: MIT
// A release store later overwritten plainly: the acquire synchronizes with nothing.
// Expected: race on y.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    auto* x = new (csan_alloc(sizeof(std::atomic<char>))) std::atomic<char>{0};
    char* y = static_cast<char*>(csan_alloc(8));
    volatile int go = 0; // plain HWCC flag, used only to order the test

    std::thread actor([&] {
        y[0] = 42;                                // write y
        x->store(1, std::memory_order_release);    // release write x
        char* raw = reinterpret_cast<char*>(x);
        *raw = 2; // plain overwrite: invalidates the release above
        go = 1;
    });
    std::thread reader([&] {
        while (!go) {
            std::this_thread::yield();
        }
        volatile char v = x->load(std::memory_order_acquire); // stale acquire
        (void)v;
        volatile char w = y[0]; // read y: must NOT be visible without hb
        (void)w;
    });
    actor.join();
    reader.join();
    return csan_test_finish(true);
}
