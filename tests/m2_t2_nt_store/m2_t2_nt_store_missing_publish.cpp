// SPDX-License-Identifier: MIT
// Machine 0 writes with a non-temporal store and no sfence.
// Expected: race (an NT store still needs sfence).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <emmintrin.h>
#include <immintrin.h>

namespace {
struct Ctx {
    long long* x;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->x);
    _mm_mfence();
    volatile long long v = *c->x;
    (void)v;
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<long long*>(csan_alloc(8));

    _mm_stream_si64(ctx.x, 42); // written back, never fenced

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
