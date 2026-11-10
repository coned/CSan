// SPDX-License-Identifier: MIT
// As _no_flush_no_race, but machine 0 never writes its bytes back: its dirty
// line can later overwrite machine 1's store.
// Expected: race (write-without-publish), not write-without-line-acquire.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdint>
#include <cstdio>

struct Ctx {
    uint64_t* x;
};

static int overwrite(void* arg) {
    *static_cast<Ctx*>(arg)->x = 2;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<uint64_t*>(csan_alloc(64));

    *ctx.x = 1; // no write-back

    pid_t child = csan_test_spawn_machine(kHosts, 1, &overwrite, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_PUBLISH)
        .no_kind(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .finish();
}
