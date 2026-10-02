// SPDX-License-Identifier: MIT
// Release on machine 0, acquire on machine 1, with the data written back and flushed around it.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <new>

namespace {
struct Ctx {
    char* data;
    std::atomic<int>* flag;
};

int consumer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    while (c->flag->load(std::memory_order_acquire) != 1) {
    }
    _mm_clflushopt(c->data);
    _mm_mfence();
    volatile char v = c->data[0];
    if (v != 42) {
        std::fprintf(stderr, "FAIL data = %d\n", v);
        return 1;
    }
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.data = static_cast<char*>(csan_alloc(64));
    ctx.flag = new (csan_alloc(64)) std::atomic<int>(0);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);

    ctx.data[0] = 42;
    _mm_clwb(ctx.data);
    _mm_sfence();
    ctx.flag->store(1, std::memory_order_release); // acquired on machine 1

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
