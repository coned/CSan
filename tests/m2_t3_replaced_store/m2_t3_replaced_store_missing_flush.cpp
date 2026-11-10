// SPDX-License-Identifier: MIT
// The detection section's replaced store, without the flush:
//   t1 (M0): y = 1; clwb; sfence                 (then releases to t2 and t3)
//   t2 (M1): y = 2                               (after t1)
//   t3 (M1): x = 1                               (after t1; unordered with t2)
// y's last store is M1's own, but t3 is not ordered after it, so M1's copy
// of y may still be older than y = 1 when t3's store dirties the line.
// Expected: race (write-without-line-acquire); the old rule skipped a byte
// whose last store was from the storing machine.
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
    std::atomic<int>* to_t3; // t1 -> t3, release/acquire
    std::atomic<int>* t2_done; // t2 -> t3, relaxed: no ordering
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    std::thread t3([c] {
        while (c->to_t3->load(std::memory_order_acquire) == 0 ||
               c->t2_done->load(std::memory_order_relaxed) == 0) {
            _mm_pause();
        }
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
    ctx.to_t3 = new (base + 128) std::atomic<int>(0);
    ctx.t2_done = new (base + 192) std::atomic<int>(0);
    _mm_clwb(base + 64);
    _mm_clwb(base + 128);
    _mm_clwb(base + 192);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    ctx.line[1] = 1;
    _mm_clwb(ctx.line);
    _mm_sfence();
    ctx.to_t2->store(1, std::memory_order_release);
    ctx.to_t3->store(1, std::memory_order_release);

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).races(1).finish();
}
