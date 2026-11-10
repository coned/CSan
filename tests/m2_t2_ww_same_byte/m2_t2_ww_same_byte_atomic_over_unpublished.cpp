// SPDX-License-Identifier: MIT
// Machine 0 writes a word plainly and never writes it back; machine 1 then
// stores the same word atomically. The runtime publishes atomic stores, not
// the plain one before it, whose dirty line can still overwrite the atomic.
// Expected: race (write-without-publish).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdint>
#include <cstdio>

struct Ctx {
    uint64_t* x;
};

static int store_atomic(void* arg) {
    auto* a = reinterpret_cast<std::atomic<uint64_t>*>(static_cast<Ctx*>(arg)->x);
    a->store(2, std::memory_order_relaxed);
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<uint64_t*>(csan_alloc(64));

    *ctx.x = 1; // plain, no write-back

    pid_t child = csan_test_spawn_machine(kHosts, 1, &store_atomic, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_PUBLISH).finish();
}
