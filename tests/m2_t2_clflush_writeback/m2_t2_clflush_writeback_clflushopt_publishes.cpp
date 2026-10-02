// SPDX-License-Identifier: MIT
// The publisher uses clflushopt + sfence instead of clwb; a flush also writes the dirty line back.
// Expected: no race.
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
    _mm_clflushopt(c->x);
    _mm_mfence();
    return c->x[0] == 42 ? 0 : 2;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.x[0] = 42;
    _mm_clflushopt(ctx.x); // WriteBack + Flush ...
    _mm_sfence();          // ... and the fence CLFLUSH would not have needed

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    int rc = csan_test_wait(child);
    if (rc != 0) {
        std::fprintf(stderr, "machine 1 read the wrong value (rc=%d)\n", rc);
        return 1;
    }
    return csan_expect_no_race()
        .no_kind(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE)
        .finish();
}
