// SPDX-License-Identifier: MIT
// 32 threads do relaxed fetch_adds on one counter, then a joined plain read.
// Expected: no race, and the counter exact.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <new>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kThreads = 32;
constexpr uint32_t kIters = 1000;
}

int main() {
    csan_init(1, 0, 1u << 16);
    auto* counter = new (csan_alloc(sizeof(std::atomic<uint64_t>))) std::atomic<uint64_t>(0);

    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            for (uint32_t k = 0; k < kIters; ++k) {
                counter->fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }
    uint64_t v = *reinterpret_cast<volatile uint64_t*>(counter); // plain, after every join
    if (v != static_cast<uint64_t>(kThreads) * kIters) {
        std::fprintf(stderr, "FAIL counter %llu\n", static_cast<unsigned long long>(v));
        return 1;
    }
    return csan_test_finish(false);
}
