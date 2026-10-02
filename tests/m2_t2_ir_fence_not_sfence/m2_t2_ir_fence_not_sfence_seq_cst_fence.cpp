// SPDX-License-Identifier: MIT
// The reader completes its flush with a C++ seq_cst fence, which emits mfence.
// Expected: no race.
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
    std::atomic_thread_fence(std::memory_order_seq_cst); // compiles to mfence
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
    _mm_sfence();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
