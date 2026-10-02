// SPDX-License-Identifier: MIT
// Two machines write different bytes of one line, ordered by release/acquire, with the write-back chain.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

struct Ctx {
    char* x;
    std::atomic<int>* go;
};

static int writer2(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    while (c->go->load(std::memory_order_acquire) == 0) {
    }
    _mm_clflushopt(c->x);
    _mm_mfence();
    c->x[8] = 2;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(64));
    ctx.go = new (csan_alloc(sizeof(std::atomic<int>))) std::atomic<int>(0);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer2, &ctx);
    ctx.x[0] = 1;
    _mm_clwb(ctx.x);
    _mm_sfence();
    ctx.go->store(1, std::memory_order_release);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
