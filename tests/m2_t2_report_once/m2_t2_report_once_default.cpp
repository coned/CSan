// SPDX-License-Identifier: MIT
// CSAN_REPORT_ONCE (on by default): a byte's checks stop after its first finding.
//   M0: x = 1; clwb; sfence                      (then releases to M1)
//   M1: a = x; a = x; a = x                      (no flush: three stale reads)
// Expected: one race (read-without-byte-acquire), not three.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;

struct Ctx {
    volatile char* x;
    std::atomic<int>* released; // M0 -> M1, release/acquire
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    while (c->released->load(std::memory_order_acquire) == 0) {
        _mm_pause();
    }
    for (int i = 0; i < 3; ++i) {
        char a = *c->x;
        (void)a;
    }
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(2 * 64));
    Ctx ctx;
    ctx.x = base;
    ctx.released = new (base + 64) std::atomic<int>(0);
    _mm_clwb(base + 64);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    *ctx.x = 1;
    _mm_clwb(base);
    _mm_sfence();
    ctx.released->store(1, std::memory_order_release);

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).races(1).finish();
}
