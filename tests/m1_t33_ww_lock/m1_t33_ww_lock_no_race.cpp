// SPDX-License-Identifier: MIT
// 32 threads increment one counter under a std::mutex.
// Expected: no race, and the counter exact.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kThreads = 32;
constexpr uint32_t kIters = 200;
}

int main() {
    csan_init(1, 0, 1u << 16);
    auto* counter = static_cast<volatile uint64_t*>(csan_alloc(8));
    *counter = 0;
    std::mutex m;

    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kThreads; ++i) {
        ts.emplace_back([&] {
            for (uint32_t k = 0; k < kIters; ++k) {
                std::lock_guard<std::mutex> g(m);
                *counter = *counter + 1;
            }
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }
    if (*counter != static_cast<uint64_t>(kThreads) * kIters) {
        std::fprintf(stderr, "FAIL counter %llu\n", static_cast<unsigned long long>(*counter));
        return 1;
    }
    return csan_test_finish(false);
}
