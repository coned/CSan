// SPDX-License-Identifier: MIT
// A worker writes x and is joined before the main thread reads it.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));

    std::thread t([&] { x[0] = 42; }); // write x in a worker thread
    t.join();                           // happens-before via join
    volatile char v = x[0];             // read x
    (void)v;
    return csan_test_finish(false);
}
