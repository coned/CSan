// SPDX-License-Identifier: MIT
// Machine 0 publishes with a non-temporal store + sfence, no clwb.
// Expected: no race (an NT store implies the write-back).
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
    if (v != 42) {
        std::fprintf(stderr, "FAIL x = %lld\n", v);
        return 1;
    }
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<long long*>(csan_alloc(8));

    _mm_stream_si64(ctx.x, 42); // Write + WriteBack, no clwb
    _mm_sfence();               // publishes it

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
