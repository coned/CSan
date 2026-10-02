// SPDX-License-Identifier: MIT
// Two atomics on one line; machine 1 flushes before machine 0 publishes, then stores.
// Expected: race (atomic-write-without-line-acquire); the trace says the flush acquired nothing.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;

struct Pair {
    std::atomic<uint64_t> a;
    std::atomic<uint64_t> b;
};

int early_flush(void* arg) {
    Pair* p = static_cast<Pair*>(arg);
    _mm_clflushopt(p);
    _mm_sfence();
    return 0;
}

int writer_b(void* arg) {
    Pair* p = static_cast<Pair*>(arg);
    p->b.store(2, std::memory_order_relaxed);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    void* raw = csan_alloc(64);
    Pair* p = new (raw) Pair();
    p->a.store(0, std::memory_order_relaxed);
    p->b.store(0, std::memory_order_relaxed);

    p->a.store(1, std::memory_order_relaxed);

    pid_t early = csan_test_spawn_machine(kHosts, 1, &early_flush, p);
    if (csan_test_wait(early) != 0) {
        std::fprintf(stderr, "machine 1 flush role failed\n");
        return 1;
    }

    _mm_clwb(p);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer_b, p);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
