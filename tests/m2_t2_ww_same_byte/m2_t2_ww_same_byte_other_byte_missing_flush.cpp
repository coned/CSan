// SPDX-License-Identifier: MIT
// Machine 0 writes and publishes bytes 0 and 8; machine 1 overwrites byte 0
// only, with no flush. Its write-back carries a stale copy of byte 8.
// Expected: race (write-without-line-acquire), not write-without-publish.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* line;
};

static int overwrite(void* arg) {
    static_cast<Ctx*>(arg)->line[0] = 2;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.line = static_cast<char*>(csan_alloc(64));

    ctx.line[0] = 1;
    ctx.line[8] = 1;
    _mm_clwb(ctx.line);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &overwrite, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .no_kind(CSAN_RACE_WRITE_WITHOUT_PUBLISH)
        .finish();
}
