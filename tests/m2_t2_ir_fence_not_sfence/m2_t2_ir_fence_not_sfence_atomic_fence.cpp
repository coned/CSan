// SPDX-License-Identifier: MIT
// The publisher orders clwb with a C++ release fence, which emits no instruction.
// Expected: race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* data;
};

static int consumer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->data);
    _mm_mfence();
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
    std::atomic_thread_fence(std::memory_order_release); // NOT a store fence

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(true);
}
