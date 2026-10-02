// SPDX-License-Identifier: MIT
// A relaxed fetch_add (lock xadd, a full barrier) stands in for the reader's mfence.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;

struct Shared {
    volatile uint64_t* x;        // line 0: published by machine 0
    std::atomic<uint64_t>* ctr;  // line 1: the MFenced operation's target
};

int reader(void* arg) {
    Shared* s = static_cast<Shared*>(arg);
    _mm_clflushopt(const_cast<uint64_t*>(s->x)); // drop the stale copy ...
    s->ctr->fetch_add(1, std::memory_order_relaxed); // ... `lock xadd` completes it
    volatile uint64_t v = *s->x;
    (void)v;
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Shared s;
    s.x = static_cast<volatile uint64_t*>(csan_alloc(64));
    s.ctr = new (csan_alloc(64)) std::atomic<uint64_t>(0);

    *s.x = 1;
    _mm_clwb(const_cast<uint64_t*>(s.x));
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &s);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
