// SPDX-License-Identifier: MIT
// Machine 1 reads x after clflush alone; clflush implies sfence, which is not enough for a read.
// Expected: race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

namespace {
struct Ctx {
    char* x;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflush(c->x);         // Flush;SFence -- no byte acquisition
    volatile char v = c->x[0]; // a read needs byte acquisition, which only mfence completes
    (void)v;
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.x[0] = 42;
    _mm_clwb(ctx.x);
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
