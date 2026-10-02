// SPDX-License-Identifier: MIT
// Each machine locks its own process-local mutex; nothing is published or acquired.
// Expected: race (read-without-byte-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <mutex>

struct Ctx {
    char* x;
};

static int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    std::mutex m; // this process's own HWCC lock, not shared with machine 0
    m.lock();
    volatile char v = c->x[0]; // read x
    (void)v;
    m.unlock();
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    std::mutex m; // machine 0's local HWCC lock
    m.lock();
    ctx.x[0] = 42; // write x
    m.unlock();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
