// SPDX-License-Identifier: MIT
// Release/acquire atomics alone across machines, with no write-back, flush or fence.
// Expected: race (atomics give no ordering across coherence domains).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <new>
#include <cstdio>

struct Ctx {
    char* data;
    std::atomic<int>* flag;
};

static int consumer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    // One acquire load, no spin: whichever way the two machines interleave,
    // there is no edge, so either this read or machine 0's write is reported.
    (void)c->flag->load(std::memory_order_acquire);
    volatile char v = c->data[0]; // no acquisition chain, and no real edge
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.data = static_cast<char*>(csan_alloc(8));
    ctx.flag = new (csan_alloc(sizeof(std::atomic<int>))) std::atomic<int>(0);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);
    ctx.data[0] = 42;
    ctx.flag->store(1, std::memory_order_release);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(true);
}
