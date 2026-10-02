// SPDX-License-Identifier: MIT
// Machine 1 overwrites a whole line whose write-back was never fenced.
// Expected: race (write-without-line-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstring>
#include <immintrin.h>

struct Ctx {
    char* line;
};

static int writer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    char buf[64];
    std::memset(buf, 7, sizeof(buf));
    std::memcpy(c->line, buf, 64); // all 64 bytes, as in the whole variant
    _mm_clwb(c->line);
    _mm_sfence();
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.line = static_cast<char*>(csan_alloc(64));

    std::memset(ctx.line, 1, 64); // machine 0 writes the line
    _mm_clwb(ctx.line);           // write back recorded ...
    // missing: _mm_sfence();     // ... and never finished

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
