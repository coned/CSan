// SPDX-License-Identifier: MIT
// fig:example-read twice over: t2 acquires x, t1 stores x again (which drops
// t2's acquisition) and publishes it, and t2's second flush must start a new
// acquisition rather than pass as a repeat of the first.
//   t1 (M0): x = 1; clwb; sfence; release 1; wait 2; x = 2; clwb; sfence; release 3
//   t2 (M1): wait 1; clflushopt; mfence; a = x; release 2; wait 3; clflushopt; mfence; a = x
// Expected: no race.
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
    std::atomic<int>* step;
};

void wait_for(std::atomic<int>* s, int v) {
    while (s->load(std::memory_order_acquire) != v) {
        _mm_pause();
    }
}

void release(std::atomic<int>* s, int v) {
    s->store(v, std::memory_order_release);
}

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    wait_for(c->step, 1);
    _mm_clflushopt(const_cast<char*>(c->x));
    _mm_mfence();
    char a = *c->x;
    release(c->step, 2);
    wait_for(c->step, 3);
    _mm_clflushopt(const_cast<char*>(c->x));
    _mm_mfence();
    a = static_cast<char>(a + *c->x);
    (void)a;
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(2 * 64));
    Ctx ctx;
    ctx.x = base;
    ctx.step = new (base + 64) std::atomic<int>(0);
    _mm_clwb(base + 64);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    *ctx.x = 1;
    _mm_clwb(base);
    _mm_sfence();
    release(ctx.step, 1);
    wait_for(ctx.step, 2);
    *ctx.x = 2;
    _mm_clwb(base);
    _mm_sfence();
    release(ctx.step, 3);

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
