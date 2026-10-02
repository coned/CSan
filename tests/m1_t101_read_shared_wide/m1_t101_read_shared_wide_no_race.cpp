// SPDX-License-Identifier: MIT
// 100 readers then a joined writer, run at CSAN_THREADS=128 (wider than the default clock).
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <thread>
#include <vector>

namespace {
constexpr uint32_t kReaders = 100;
}

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    x[0] = 1;

    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kReaders; ++i) {
        ts.emplace_back([&] {
            volatile char v = x[0];
            (void)v;
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }
    x[0] = 2;
    return csan_test_finish(false);
}
