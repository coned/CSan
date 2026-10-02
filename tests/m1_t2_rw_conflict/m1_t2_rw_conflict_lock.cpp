// SPDX-License-Identifier: MIT
// Write and read of x each under one std::mutex.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <mutex>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    std::mutex m;

    std::thread writer([&] {
        m.lock();
        x[0] = 42; // write x
        m.unlock();
    });
    std::thread reader([&] {
        m.lock();
        volatile char v = x[0]; // read x
        (void)v;
        m.unlock();
    });
    writer.join();
    reader.join();
    return csan_test_finish(false);
}
