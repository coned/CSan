// SPDX-License-Identifier: MIT
// Full protocol except machine 0 omits clwb.
// Expected: race (write-without-line-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* x;
};

static int writer2(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->x); // flushopt line
    _mm_mfence();         // acquire line
    c->x[8] = 2;          // write y (byte 8, same line)
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(16));

    ctx.x[0] = 1; // write x (byte 0)
    // missing: _mm_clwb(ctx.x);  // writeback line
    _mm_sfence(); // publish line

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer2, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
