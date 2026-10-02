// SPDX-License-Identifier: MIT
// Each line's clwb is followed by a C++ fence of a different order; only seq_cst emits mfence.
// Expected: races on every line except the seq_cst one.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>

constexpr int kOrders = 6;

struct Ctx {
    char* lines; // kOrders lines of 64 bytes
};

static int writer2(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    for (int i = 0; i < kOrders; ++i) {
        char* line = c->lines + 64 * i;
        _mm_clflushopt(line);
        _mm_mfence();
        line[8] = 2;
    }
    return 0;
}

static void write_wb(char* line) {
    line[0] = 1;
    _mm_clwb(line);
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.lines = static_cast<char*>(csan_alloc(64 * kOrders));

    write_wb(ctx.lines + 64 * 0);
    std::atomic_thread_fence(std::memory_order_seq_cst); // mfence: published
    write_wb(ctx.lines + 64 * 1);
    std::atomic_thread_fence(std::memory_order_relaxed);
    write_wb(ctx.lines + 64 * 2);
    std::atomic_thread_fence(std::memory_order_consume);
    write_wb(ctx.lines + 64 * 3);
    std::atomic_thread_fence(std::memory_order_acquire);
    write_wb(ctx.lines + 64 * 4);
    std::atomic_thread_fence(std::memory_order_release);
    write_wb(ctx.lines + 64 * 5);
    std::atomic_thread_fence(std::memory_order_acq_rel);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer2, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).races(kOrders - 1).finish();
}
