// SPDX-License-Identifier: MIT
// Write and read of x with no synchronization.
// Expected: race (read-before-hb-write).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    volatile int go = 0;

    std::thread writer([&] {
        x[0] = 42; // write x
        go = 1;
    });
    std::thread reader([&] {
        while (!go) {
            std::this_thread::yield();
        }
        volatile char v = x[0]; // read x
        (void)v;
    });
    writer.join();
    reader.join();
    return csan_expect_race(CSAN_RACE_READ_BEFORE_HB_WRITE).finish();
}
