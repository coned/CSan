// SPDX-License-Identifier: MIT
// The line has no single last writer to order against: machine 1's
// acquisition of byte 0 still holds after machine 0 rewrites byte 1.
//   M0: line[0], line[1] = 1; clwb; sfence          (fork orders M1 after)
//   M1: clflushopt; sfence                           (acquires bytes 0 and 1)
//   M0: line[1] = 2; clwb; sfence                    (after M1's flag)
//   M1: line[1] = 3                                  (after M0's flag; no flush)
// Byte 1 is overwritten whole and was published; byte 0 is unchanged since
// M1 acquired it. Nothing stale can ride M1's write-back.
// Expected: no race (the old line rule reported write-without-line-acquire:
// any write to the line erased the line acquisition).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;

struct Ctx {
    char* line;
    std::atomic<int>* acquired; // M1 -> M0, its own line
    std::atomic<int>* rewritten; // M0 -> M1, its own line
};

int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->line);
    _mm_sfence();
    c->acquired->store(1, std::memory_order_release);
    while (c->rewritten->load(std::memory_order_acquire) == 0) {
        _mm_pause();
    }
    c->line[1] = 3;
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(3 * 64));
    Ctx ctx;
    ctx.line = base;
    ctx.acquired = new (base + 64) std::atomic<int>(0);
    ctx.rewritten = new (base + 128) std::atomic<int>(0);

    ctx.line[0] = 1;
    ctx.line[1] = 1;
    _mm_clwb(ctx.line);
    _mm_clwb(base + 64); // the flag machine 1 stores to: its constructor's plain store
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    while (ctx.acquired->load(std::memory_order_acquire) == 0) {
        _mm_pause();
    }
    ctx.line[1] = 2;
    _mm_clwb(ctx.line);
    _mm_sfence();
    ctx.rewritten->store(1, std::memory_order_release);

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
