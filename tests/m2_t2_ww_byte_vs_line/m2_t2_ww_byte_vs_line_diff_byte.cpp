// SPDX-License-Identifier: MIT
// Two machines write different bytes of one line, unordered.
// Expected: race (write-before-hb-line-write), no data-race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>

struct Ctx {
    char* x;
};

static int writer2(void* arg) {
    static_cast<Ctx*>(arg)->x[8] = 2;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(64));

    // Spawned first, so nothing orders the two writes.
    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer2, &ctx);
    ctx.x[0] = 1;
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_BEFORE_HB_LINE_WRITE)
        .no_kind(CSAN_RACE_DATA_RACE)
        .finish();
}
