// SPDX-License-Identifier: MIT
// Two atomics on one line; machine 0 publishes, machine 1 flushes + sfences before its store.
// Expected: no race.
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

int writer_b(void* arg) {
    Pair* p = static_cast<Pair*>(arg);
    _mm_clflushopt(p);            // drop this machine's stale copy of the line
    _mm_sfence();                 // ... and let the store below see that
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
    _mm_clwb(p);  // publish the line ...
    _mm_sfence(); // ... and complete the publication

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer_b, p);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
