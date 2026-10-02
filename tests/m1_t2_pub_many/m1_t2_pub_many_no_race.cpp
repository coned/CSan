// SPDX-License-Identifier: MIT
// Eight threads each write back the same line under a mutex: more publication records than inline slots.
// Expected: no race, and nothing dropped.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kThreads = 8;
}

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(64));
    if (x == nullptr) {
        std::fprintf(stderr, "alloc failed\n");
        return 1;
    }
    std::mutex m;

    x[0] = 42; // one write: establishes W^L_l, and clears the relay maps

    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            m.lock();
            // Write-back without writing: adds this thread's publication record.
            _mm_clwb(x);
            _mm_sfence();
            m.unlock();
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }

    uint64_t dropped = __csan_overflow_count();
    if (dropped != 0) {
        std::fprintf(stderr, "FAIL %" PRIu64 " publication records were dropped\n", dropped);
        return 1;
    }
    return csan_test_finish(false);
}
