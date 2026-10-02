// SPDX-License-Identifier: MIT
// The flusher runs on machine 0 while the reader runs on machine 1.
// Expected: race (acquisition is per machine).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>
#include <thread>

struct Ctx {
    char* x;
    char* flag;
};

static void writer_body(Ctx* c) {
    c->x[0] = 42;         // write x
    c->flag[0] = 1;       // message: x has been written
    _mm_clwb(c->flag);    // writeback message
    _mm_sfence();         // publish message
    _mm_clwb(c->x);       // writeback x
    _mm_sfence();         // publish x
}

static void flusher_body(Ctx* c) {
    _mm_clflushopt(c->flag); // flushopt message
    _mm_mfence();            // acquire message
    while (c->flag[0] == 0) {
        std::this_thread::yield();
    }
    _mm_clflushopt(c->x); // flushopt x (on the wrong machine)
    _mm_mfence();         // acquire x (on the wrong machine)
}

static int machine1_reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    volatile char v = c->x[0]; // read x
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));
    ctx.flag = static_cast<char*>(csan_alloc(8));

    std::thread writer(writer_body, &ctx); // machine 0's own thread
    writer.join();

    // The flusher is also machine 0's thread: its acquire is machine-local
    // and does not help machine 1.
    std::thread flusher(flusher_body, &ctx);
    flusher.join();

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1_reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
