// SPDX-License-Identifier: MIT
// Machine 0 publishes one line; seven other machines each acquire and read it.
// Expected: no race on any machine.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

namespace {
constexpr uint32_t kHosts = 8;

struct Ctx {
    uint64_t* line;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->line);
    _mm_mfence();
    for (uint32_t i = 0; i < 8; ++i) {
        volatile uint64_t v = c->line[i];
        if (v != 100 + i) {
            std::fprintf(stderr, "FAIL word %u = %llu\n", i, static_cast<unsigned long long>(v));
            return 1;
        }
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
    int rc = 0;
    for (uint32_t m = 1; m < kHosts; ++m) {
        if (csan_test_wait(children[m]) != 0) {
            rc = 1;
        }
    }
    if (rc != 0) {
        return rc;
    }
    return csan_test_finish(false);
}
