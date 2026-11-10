// SPDX-License-Identifier: MIT
// One thread's passing check must not stand in for another's:
//   M1     : line[8] = 1; clwb; sfence            (then signals u2)
//   u2 (M0): acquires byte 8; line[16] = 1         (passes)
//   u  (M0): line[0] = 1                           (unordered with u2; never acquired byte 8)
// A cache shared by the machine would skip u's check.
// Expected: exactly one race, write-without-line-acquire, from u's store.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>
#include <thread>

namespace {
constexpr uint32_t kHosts = 2;

struct Ctx {
    char* line;
    std::atomic<int>* m1_done; // M1 -> u2, release/acquire
    std::atomic<int>* u2_done; // u2 -> u, relaxed: no ordering
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    c->line[8] = 1;
    _mm_clwb(c->line);
    _mm_sfence();
    c->m1_done->store(1, std::memory_order_release);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(3 * 64));
    Ctx ctx;
    ctx.line = base;
    ctx.m1_done = new (base + 64) std::atomic<int>(0);
    ctx.u2_done = new (base + 128) std::atomic<int>(0);
    _mm_clwb(base + 64);
    _mm_clwb(base + 128);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);

    std::thread u2([&] {
        while (ctx.m1_done->load(std::memory_order_acquire) == 0) {
            _mm_pause();
        }
        _mm_clflushopt(ctx.line);
        _mm_sfence();
        ctx.line[16] = 1;
        ctx.u2_done->store(1, std::memory_order_relaxed);
    });
    while (ctx.u2_done->load(std::memory_order_relaxed) == 0) {
        _mm_pause();
    }
    ctx.line[0] = 1;
    u2.join();

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).races(1).finish();
}
