// SPDX-License-Identifier: MIT
// MFinish(E_s, E_m) keeps the sfence's epoch: a thread ordered after the
// sfence but not after the later mfence still has the write acquisition.
//   M0     : line[0] = 1; clwb; sfence                (before the fork)
//   t  (M1): clflushopt; sfence (E_s); A.store(rel); mfence (E_m); B.store(rlx)
//   w  (M1): A.load(acq); B.load(rlx); line[8] = 1    (knows E_s, not E_m)
// w's store leaves byte 0, last written by M0: AcqW needs A_0[t] with
// E_s ⪯ C_w, which holds. Re-stamping the entry with E_m would fail it.
// Expected: no race.
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
    std::atomic<int>* a; // t -> w, release/acquire: orders w after t's sfence
    std::atomic<int>* b; // t -> w, relaxed: w waits for the mfence, unordered
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    std::thread w([c] {
        while (c->a->load(std::memory_order_acquire) == 0) {
            _mm_pause();
        }
        while (c->b->load(std::memory_order_relaxed) == 0) {
            _mm_pause();
        }
        c->line[8] = 1;
    });
    _mm_clflushopt(c->line);
    _mm_sfence();
    c->a->store(1, std::memory_order_release);
    _mm_mfence();
    c->b->store(1, std::memory_order_relaxed);
    w.join();
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(3 * 64));
    Ctx ctx;
    ctx.line = base;
    ctx.a = new (base + 64) std::atomic<int>(0);
    ctx.b = new (base + 128) std::atomic<int>(0);
    _mm_clwb(base + 64);
    _mm_clwb(base + 128);

    ctx.line[0] = 1;
    _mm_clwb(ctx.line);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
