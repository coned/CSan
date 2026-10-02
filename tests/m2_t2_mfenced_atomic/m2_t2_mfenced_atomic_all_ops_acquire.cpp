// SPDX-License-Identifier: MIT
// Each line's flush is followed by one atomic operation instead of mfence.
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
constexpr int kMFenced = 4; // the first four; the rest fence nothing

struct Ctx {
    char* lines;              // kOps lines of 64 bytes, the data being acquired
    std::atomic<uint64_t>* a; // the operations' own target, on a line of its own
};

int writer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    auto* a = c->a;
    uint64_t expected = 0;

    _mm_clflushopt(c->lines + 64 * 0);
    a->fetch_add(1, std::memory_order_relaxed);
    c->lines[64 * 0 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 1);
    a->exchange(2, std::memory_order_relaxed);
    c->lines[64 * 1 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 2);
    expected = 2;
    a->compare_exchange_strong(expected, 3, std::memory_order_relaxed,
                               std::memory_order_relaxed);
    c->lines[64 * 2 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 3);
    a->store(4, std::memory_order_seq_cst);
    c->lines[64 * 3 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 4);
    (void)a->load(std::memory_order_seq_cst); // strongest order, still a movq
    c->lines[64 * 4 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 5);
    (void)a->load(std::memory_order_acquire);
    c->lines[64 * 5 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 6);
    a->store(5, std::memory_order_release);
    c->lines[64 * 6 + 8] = 2;

    _mm_clflushopt(c->lines + 64 * 7);
    a->store(6, std::memory_order_relaxed);
    c->lines[64 * 7 + 8] = 2;
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.lines = static_cast<char*>(csan_alloc(64 * kOps));
    ctx.a = new (csan_alloc(64)) std::atomic<uint64_t>(0);

    for (int i = 0; i < kOps; ++i) {
        ctx.lines[64 * i] = 1;
        _mm_clwb(ctx.lines + 64 * i);
    }
    _mm_sfence(); // publishes all eight lines

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .races(kOps - kMFenced)
        .finish();
}
