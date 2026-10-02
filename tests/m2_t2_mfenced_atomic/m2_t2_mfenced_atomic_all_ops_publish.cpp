// SPDX-License-Identifier: MIT
// Each line's clwb is followed by one atomic operation instead of sfence.
// Expected: races only on lines whose operation is not a full barrier on x86.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;
constexpr int kOps = 8;
constexpr int kMFenced = 4;

struct Ctx {
    char* lines;
    std::atomic<uint64_t>* a;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    for (int i = 0; i < kOps; ++i) {
        _mm_clflushopt(c->lines + 64 * i);
        _mm_mfence();
        c->lines[64 * i + 8] = 2;
    }
    return 0;
}

void write_wb(Ctx* c, int i) {
    c->lines[64 * i] = 1;
    _mm_clwb(c->lines + 64 * i);
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.lines = static_cast<char*>(csan_alloc(64 * kOps));
    ctx.a = new (csan_alloc(64)) std::atomic<uint64_t>(0);
    auto* a = ctx.a;
    uint64_t expected = 0;

    write_wb(&ctx, 0);
    a->fetch_add(1, std::memory_order_relaxed);

    write_wb(&ctx, 1);
    a->exchange(2, std::memory_order_relaxed);

    write_wb(&ctx, 2);
    expected = 2;
    a->compare_exchange_strong(expected, 3, std::memory_order_relaxed,
                               std::memory_order_relaxed);

    write_wb(&ctx, 3);
    a->store(4, std::memory_order_seq_cst);

    write_wb(&ctx, 4);
    (void)a->load(std::memory_order_seq_cst);

    write_wb(&ctx, 5);
    (void)a->load(std::memory_order_acquire);

    write_wb(&ctx, 6);
    a->store(5, std::memory_order_release);

    write_wb(&ctx, 7);
    a->store(6, std::memory_order_relaxed);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .races(kOps - kMFenced)
        .finish();
}
