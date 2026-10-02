// SPDX-License-Identifier: MIT
// Two threads of one machine write the same byte, unordered.
// Expected: race (data-race), not write-before-hb-line-write.
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
        x[0] = 2;
    });
    writer1.join();
    writer2.join();
    return csan_expect_race(CSAN_RACE_DATA_RACE)
        .no_kind(CSAN_RACE_WRITE_BEFORE_HB_LINE_WRITE)
        .finish();
}
