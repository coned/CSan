// SPDX-License-Identifier: MIT
// Two machines write different bytes of one line, unordered. Stores to one
// line need no total order; what goes wrong is that whichever stores second
// has not flushed the other's byte, so its write-back carries a stale copy.
// Expected: race (write-without-line-acquire), and no ordering kind
// (the old rule reported write-before-hb-line-write).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>

struct Ctx {
    char* line;
};

static int writer8(void* arg) {
    static_cast<Ctx*>(arg)->line[8] = 2;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.line = static_cast<char*>(csan_alloc(64));

    // Spawned first, so nothing orders the two writes.
    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer8, &ctx);
    ctx.line[0] = 1;
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .no_kind(CSAN_RACE_DATA_RACE_WW)
        .no_kind(CSAN_RACE_RETIRED_2)
        .finish();
}
