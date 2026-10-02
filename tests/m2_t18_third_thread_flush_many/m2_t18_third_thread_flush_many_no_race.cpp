// SPDX-License-Identifier: MIT
// One flusher on machine 1 acquires x and hands it to sixteen readers over release/acquire.
// Expected: no race (acquisition is per machine, not per thread).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kReaders = 16;
std::atomic<int> g_go{0}; // machine-1-local, outside the pool

struct Ctx {
    char* x;
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    std::thread flusher([c] {
        _mm_clflushopt(c->x);
        _mm_mfence();
        g_go.store(1, std::memory_order_release);
    });
    std::vector<std::thread> readers;
    for (uint32_t i = 0; i < kReaders; ++i) {
        readers.emplace_back([c] {
            while (g_go.load(std::memory_order_acquire) == 0) {
                std::this_thread::yield();
            }
            volatile char v = c->x[0];
            (void)v;
        });
    }
    flusher.join();
    for (std::thread& t : readers) {
        t.join();
    }
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    std::thread writer([&] {
        ctx.x[0] = 42;
        _mm_clwb(ctx.x);
        _mm_sfence();
    });
    writer.join();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(false);
}
