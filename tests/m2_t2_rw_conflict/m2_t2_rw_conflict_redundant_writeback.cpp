// SPDX-License-Identifier: MIT
// A second, unfenced clwb of an already-published line does not undo the publication.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* x;
};

static int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->x);      // flushopt x
    _mm_mfence();               // acquire
    volatile char v = c->x[0]; // read x
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.x[0] = 42;   // write x
    _mm_clwb(ctx.x); // writeback #1
    _mm_sfence();    // publish (should make x Published for good)
    _mm_clwb(ctx.x); // redundant writeback #2, no second sfence follows:
                      // must not undo the publication

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(false);
}
