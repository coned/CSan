// SPDX-License-Identifier: MIT
// An unaligned clwb covers only its own line, not the 64 bytes after the operand.
// Expected: race on the word in the next line.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdint>
#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* x; // line 0, offset 60
    char* y; // line 1, offset 64
};

static int consumer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->x);
    _mm_clflushopt(c->y);
    _mm_mfence();
    volatile char v = c->y[0]; // never written back by machine 0
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    char* base = static_cast<char*>(csan_alloc(128)); // 64-byte aligned
    Ctx ctx;
    ctx.x = base + 60;
    ctx.y = base + 64;

    ctx.x[0] = 1;
    ctx.y[0] = 2;
    _mm_clwb(ctx.x); // line 0 only, from a misaligned address
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(true);
}
