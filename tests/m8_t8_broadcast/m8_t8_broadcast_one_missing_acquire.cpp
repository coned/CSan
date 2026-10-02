// SPDX-License-Identifier: MIT
// As the broadcast, but machine 5 omits its mfence.
// Expected: race on machine 5 only.
#include "csan_runtime.h"
#include "csan_test.h"

#include <immintrin.h>

namespace {
constexpr uint32_t kHosts = 8;
constexpr uint32_t kUnfenced = 5;

struct Ctx {
    uint64_t* line;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->line);
    if (csan_machine_id() != kUnfenced) {
        _mm_mfence();
    }
    for (uint32_t i = 0; i < 8; ++i) {
        volatile uint64_t v = c->line[i];
        (void)v;
    }
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.line = static_cast<uint64_t*>(csan_alloc(64));
    for (uint32_t i = 0; i < 8; ++i) {
        ctx.line[i] = 100 + i;
    }
    _mm_clwb(ctx.line);
    _mm_sfence();

    pid_t children[kHosts];
    for (uint32_t m = 1; m < kHosts; ++m) {
        children[m] = csan_test_spawn_machine(kHosts, m, &reader, &ctx);
    }
    for (uint32_t m = 1; m < kHosts; ++m) {
        if (csan_test_wait(children[m]) != 0) {
            return 1;
        }
    }
    return csan_test_finish(true);
}
