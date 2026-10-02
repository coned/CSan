// SPDX-License-Identifier: MIT
// Eight readers, joined, then a write.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kReaders = 8;
}

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    if (x == nullptr) {
        std::fprintf(stderr, "alloc failed\n");
        return 1;
    }
    x[0] = 1; // initial write by the main thread

    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kReaders; ++i) {
        ts.emplace_back([&] {
            volatile char v = x[0];
            (void)v;
        });
    }
    // Join before writing: every reader happens-before the write below.
    for (std::thread& t : ts) {
        t.join();
    }

    x[0] = 2; // write after all reads, with an edge from each of them
    return csan_test_finish(false);
}
