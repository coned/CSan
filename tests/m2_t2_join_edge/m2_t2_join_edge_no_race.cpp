// SPDX-License-Identifier: MIT
// Machine 1 writes and publishes x; machine 0 waits for it, acquires and reads.
// Expected: no race (waiting for a machine is a join).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

namespace {
struct Ctx {
    char* x;
};

int writer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    c->x[0] = 42;
    _mm_clwb(c->x);
    _mm_sfence();
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer, &ctx);
    if (csan_test_wait(child) != 0) { // the Join
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    _mm_clflushopt(ctx.x);
    _mm_mfence();
    volatile char v = ctx.x[0];
    if (v != 42) {
        std::fprintf(stderr, "FAIL x = %d\n", v);
        return 1;
    }
    return csan_test_finish(false);
}
