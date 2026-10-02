// SPDX-License-Identifier: MIT
// Machine 0 writes x and machine 1 reads it with no publish or acquire.
// Expected: race (read-without-byte-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>

struct Ctx {
    char* x;
};

static int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    volatile char v = c->x[0]; // read x
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.x[0] = 42; // write x (never published)

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
