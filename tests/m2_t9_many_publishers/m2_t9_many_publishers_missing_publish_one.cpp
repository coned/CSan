// SPDX-License-Identifier: MIT
// Eight writers publish one line; the last one omits its sfence.
// Expected: race on that writer's word only.
#include "csan_runtime.h"
#include "csan_test.h"

#include <immintrin.h>
#include <mutex>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t kFenced = 7;

struct Ctx {
    uint64_t* line;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->line);
    _mm_mfence();
    for (uint32_t i = 0; i < 8; ++i) {
        volatile uint64_t v = c->line[i];
        (void)v;
    }
    return 0;
}
} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.line = static_cast<uint64_t*>(csan_alloc(64));
    std::mutex m;

    std::vector<std::thread> ts;
    for (uint32_t i = 0; i < kFenced; ++i) {
        ts.emplace_back([&, i] {
            std::lock_guard<std::mutex> g(m);
            ctx.line[i] = i + 1;
            _mm_clwb(&ctx.line[i]);
            _mm_sfence();
        });
    }
    for (std::thread& t : ts) {
        t.join();
    }
    std::thread last([&] {
        ctx.line[kFenced] = kFenced + 1;
        _mm_clwb(&ctx.line[kFenced]); // written back, never fenced
    });
    last.join();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        return 1;
    }
    return csan_test_finish(true);
}
