// SPDX-License-Identifier: MIT
// Two unsynchronized writes to x.
// Expected: race (data-race).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    volatile int go = 0;

    std::thread writer1([&] {
        x[0] = 1; // write x
        go = 1;
    });
    std::thread writer2([&] {
        while (!go) {
            std::this_thread::yield();
        }
        x[0] = 2; // write x
    });
    writer1.join();
    writer2.join();
    return csan_expect_race(CSAN_RACE_DATA_RACE).finish();
}
