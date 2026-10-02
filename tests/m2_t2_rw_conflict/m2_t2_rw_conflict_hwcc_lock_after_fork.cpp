// SPDX-License-Identifier: MIT
// Each machine locks its own process-local mutex copy at the same address after fork.
// Expected: race (different locks give no order).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>
#include <mutex>
#include <unistd.h>

namespace {
std::mutex g_local; // one per process after fork, same address in both

struct Ctx {
    char* x;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    usleep(100 * 1000); // let machine 0 finish its locked write first
    g_local.lock();
    _mm_clflushopt(c->x);
    _mm_mfence();
    volatile char v = c->x[0];
    (void)v;
    g_local.unlock();
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);

    g_local.lock();
    ctx.x[0] = 42; // after the fork: no edge to the child
    _mm_clwb(ctx.x);
    _mm_sfence();
    g_local.unlock();

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(true);
}
