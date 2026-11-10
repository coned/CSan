// SPDX-License-Identifier: MIT
// A line only one machine ever stores to needs no maintenance, even when its
// threads store to it unordered:
//   u  (M0): line[1] = 1
//   u2 (M0): line[0] = 1                         (unordered with u)
// No other machine's value of line[1] exists to be stale.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <immintrin.h>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(2 * 64));
    char* line = base;
    auto* u_done = new (base + 64) std::atomic<int>(0); // u -> u2, relaxed: no ordering

    std::thread u2([&] {
        while (u_done->load(std::memory_order_relaxed) == 0) {
            _mm_pause();
        }
        line[0] = 1;
    });
    line[1] = 1;
    u_done->store(1, std::memory_order_relaxed);
    u2.join();
    return csan_expect_no_race().finish();
}
