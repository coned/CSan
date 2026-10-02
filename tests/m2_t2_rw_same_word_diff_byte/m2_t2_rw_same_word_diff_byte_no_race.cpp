// SPDX-License-Identifier: MIT
// Machine 0 writes byte 0 and machine 1 reads byte 3 of the same 8-byte word.
// Expected: no race (state is tracked per byte).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>

struct Ctx {
    char* x;
};

static int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    volatile char v = c->x[3]; // byte 3 of the same word machine 0 wrote byte 0 of
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(16));

    ctx.x[0] = 42; // byte 0

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(false);
}
