// SPDX-License-Identifier: MIT
// Machine 0 writes and publishes 8 bytes; machine 1 overwrites exactly those
// bytes with no flush. Nothing else on the line was ever written, so no stale
// byte can ride machine 1's write-back.
// Expected: no race (the old line rule reported write-without-line-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdint>
#include <cstdio>
#include <immintrin.h>

struct Ctx {
    uint64_t* x;
};

static int overwrite(void* arg) {
    *static_cast<Ctx*>(arg)->x = 2; // the same 8 bytes, no flush
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<uint64_t*>(csan_alloc(64));

    *ctx.x = 1;
    _mm_clwb(ctx.x);
    _mm_sfence(); // published before the fork orders machine 1 after it

    pid_t child = csan_test_spawn_machine(kHosts, 1, &overwrite, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
