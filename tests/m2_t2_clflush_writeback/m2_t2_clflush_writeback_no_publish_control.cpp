// SPDX-License-Identifier: MIT
// Control: the publisher issues no write-back at all.
// Expected: race (read-without-byte-acquire).
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
    // The value is still 42 -- this machine is cache coherent. What is reported
    // is that nothing in the program made it so.
    return c->x[0] == 42 ? 0 : 2;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.x[0] = 42; // and no write-back of any kind

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    int rc = csan_test_wait(child);
    if (rc != 0) {
        std::fprintf(stderr, "machine 1 read the wrong value (rc=%d)\n", rc);
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
