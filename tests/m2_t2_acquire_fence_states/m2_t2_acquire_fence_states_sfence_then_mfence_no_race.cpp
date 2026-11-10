// SPDX-License-Identifier: MIT
// Machine 1 flushes, sfences (SFinish), then mfences before reading. The
// mfence must finish the acquisition the sfence already moved to SFinish,
// not only acquisitions still pending.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* line;
};

static int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->line);
    _mm_sfence();
    _mm_mfence();
    volatile char v = c->line[0];
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.line = static_cast<char*>(csan_alloc(64));

    ctx.line[0] = 1;
    _mm_clwb(ctx.line);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
