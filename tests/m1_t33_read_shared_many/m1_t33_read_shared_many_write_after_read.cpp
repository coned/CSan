// SPDX-License-Identifier: MIT
// 32 readers and a writer that does not join them.
// Expected: race (write after read).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kReaders = 32;
}

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    x[0] = 1;

    std::atomic<uint32_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kReaders; ++i) {
        ts.emplace_back([&] {
            volatile char v = x[0];
            (void)v;
            ready.fetch_add(1, std::memory_order_relaxed);
            while (!go.load(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
        });
    }
    while (ready.load(std::memory_order_relaxed) < kReaders) {
        std::this_thread::yield();
    }
    x[0] = 2; // concurrent with all 32 reads
    go.store(true, std::memory_order_relaxed);
    for (std::thread& t : ts) {
        t.join();
    }
    return csan_expect_race(CSAN_RACE_WRITE_AFTER_READ).finish();
}
