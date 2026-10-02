// SPDX-License-Identifier: MIT
// Two threads of one machine write different bytes of one line, unordered.
// Expected: no race (the caches are coherent).
#include "csan_runtime.h"
#include "csan_test.h"

#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(64));
    volatile int go = 0;

    std::thread writer1([&] {
        x[0] = 1;
        go = 1;
    });
    std::thread writer2([&] {
        while (!go) {
            std::this_thread::yield();
        }
        x[8] = 2;
    });
    writer1.join();
    writer2.join();
    return csan_expect_no_race().finish();
}
