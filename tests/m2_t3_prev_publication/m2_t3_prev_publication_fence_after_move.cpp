// SPDX-License-Identifier: MIT
// A fence finishes a write-back whose store has since been replaced:
//   t1 (M0): y = 1; clwb                         (then lets t2 go)
//   t2 (M1): y = 2                               (moves y = 1 and its pending P_y to W'_y, P'_y)
//   t1 (M0): sfence                              (after t2's store, not ordered with it)
//   t3 (M1): clflushopt; sfence; x = 1           (after t1's sfence; unordered with t2)
// t2's store already broke the write byte chain (y = 1 unpublished when it
// was overwritten), so that one is reported. The later sfence finishes
// P'_y[t1], so t3's flush acquires y = 1 (CanAcq by PubPrev): t3's store is not.
// Expected: race (write-without-publish) only.
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
    char* line; // x = line[0], y = line[1]
    std::atomic<int>* to_t2; // t1 -> t2, release/acquire
    std::atomic<int>* t2_done; // t2 -> t1, relaxed: no ordering
    std::atomic<int>* to_t3; // t1 -> t3, release/acquire, after t1's sfence
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    std::thread t3([c] {
        while (c->to_t3->load(std::memory_order_acquire) == 0) {
            _mm_pause();
        }
        _mm_clflushopt(c->line);
        _mm_sfence();
        c->line[0] = 1;
    });
    while (c->to_t2->load(std::memory_order_acquire) == 0) {
        _mm_pause();
    }
    c->line[1] = 2;
    c->t2_done->store(1, std::memory_order_relaxed);
    t3.join();
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(4 * 64));
    Ctx ctx;
    ctx.line = base;
    ctx.to_t2 = new (base + 64) std::atomic<int>(0);
    ctx.t2_done = new (base + 128) std::atomic<int>(0);
    ctx.to_t3 = new (base + 192) std::atomic<int>(0);
    _mm_clwb(base + 64);
    _mm_clwb(base + 128);
    _mm_clwb(base + 192);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    ctx.line[1] = 1;
    _mm_clwb(ctx.line);
    ctx.to_t2->store(1, std::memory_order_release);
    while (ctx.t2_done->load(std::memory_order_relaxed) == 0) {
        _mm_pause();
    }
    _mm_sfence();
    ctx.to_t3->store(1, std::memory_order_release);

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_PUBLISH)
        .no_kind(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .races(1)
        .finish();
}
