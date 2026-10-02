// SPDX-License-Identifier: MIT
// Two lines written back and completed by one sfence; machine 1 reads both without flushing.
// Expected: two races (read-without-byte-acquire); each trace shows the shared fence.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdint>
#include <cstdio>
#include <immintrin.h>

namespace {
constexpr uint32_t kHosts = 2;

struct TwoLines {
    alignas(64) uint64_t x;
    alignas(64) uint64_t y;
};

int reader(void* arg) {
    TwoLines* p = static_cast<TwoLines*>(arg);
    volatile uint64_t a = p->x;
    volatile uint64_t b = p->y;
    (void)a;
    (void)b;
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    TwoLines* p = static_cast<TwoLines*>(csan_alloc(sizeof(TwoLines)));

    p->x = 1;
    p->y = 1;
    _mm_clwb(&p->x);
    _mm_clwb(&p->y);
    _mm_sfence(); // finishes both publications

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, p);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).races(2).finish();
}
