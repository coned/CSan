// SPDX-License-Identifier: MIT
// Cross-machine handoff using only fork happens-before plus clwb/sfence and clflushopt/mfence.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* data;
};

static int consumer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->data);
    _mm_mfence(); // completes byte acquisition
    volatile char v = c->data[0];
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.data = static_cast<char*>(csan_alloc(8));

    ctx.data[0] = 42;
    _mm_clwb(ctx.data);
    _mm_sfence(); // completes publication

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(false);
}
