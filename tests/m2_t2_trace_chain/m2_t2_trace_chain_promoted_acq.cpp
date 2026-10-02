// SPDX-License-Identifier: MIT
// Five threads on machine 1 flush a published line without fencing, then machine 1 writes it.
// Expected: race (write-without-line-acquire); the trace lists all five unfenced flushes.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdint>
#include <cstdio>
#include <immintrin.h>
#include <thread>

namespace {
constexpr uint32_t kHosts = 2;
constexpr int kFlushers = 5;

struct Pair {
    alignas(64) uint64_t a;
    uint64_t b;
};

void flush_only(Pair* p) {
    _mm_clflushopt(p);
}

int writer(void* arg) {
    Pair* p = static_cast<Pair*>(arg);
    std::thread flushers[kFlushers];
    for (int i = 0; i < kFlushers; ++i) {
        flushers[i] = std::thread(flush_only, p);
    }
    for (int i = 0; i < kFlushers; ++i) {
        flushers[i].join();
    }
    p->b = 2;
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Pair* p = static_cast<Pair*>(csan_alloc(sizeof(Pair)));
    p->a = 1;
    _mm_clwb(p);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer, p);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
