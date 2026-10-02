// SPDX-License-Identifier: MIT
// Unsynchronized write and read on memory from csan_realloc: it must still be checked.
// Expected: race (read-before-hb-write).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstring>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);

    char* small = static_cast<char*>(csan_alloc(8));
    if (small == nullptr) {
        std::fprintf(stderr, "alloc failed\n");
        return 1;
    }
    std::memset(small, 0, 8);
    char* x = static_cast<char*>(csan_realloc(small, 64));
    if (x == nullptr) {
        std::fprintf(stderr, "realloc failed\n");
        return 1;
    }

    // go is outside the pool: it orders the test but supplies no edge.
    volatile int go = 0;

    std::thread writer([&] {
        x[0] = 42; // write the reallocated buffer
        go = 1;
    });
    std::thread reader([&] {
        while (!go) {
            std::this_thread::yield();
        }
        volatile char v = x[0]; // read it, with no edge from the write
        (void)v;
    });
    writer.join();
    reader.join();
    return csan_expect_race(CSAN_RACE_READ_BEFORE_HB_WRITE).finish();
}
