// SPDX-License-Identifier: MIT
// Machine 1 acquires with clflush alone, then writes x; clflush implies an sfence.
// Expected: no race.
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
    _mm_clflush(c->x); // Flush;SFence -- completes the LINE acquisition alone
    c->x[0] = 7;       // write the line machine 0 last wrote
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

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().no_kind(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
