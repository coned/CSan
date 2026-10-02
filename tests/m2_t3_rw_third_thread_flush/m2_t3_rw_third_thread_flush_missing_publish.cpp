// SPDX-License-Identifier: MIT
// The writer thread on machine 0 omits the sfence for x.
// Expected: race (read-without-byte-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include <thread>

// Machine-1-local handshake (HWCC, outside the CXL pool, so the detector
// ignores its data races): the flusher releases it after acquiring x; the
// reader acquires it before reading x.
static std::atomic<int> g_go{0};

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
    // missing: _mm_sfence();  // publish x
}

static void flusher_body(Ctx* c) {
    _mm_clflushopt(c->flag); // flushopt message
    _mm_mfence();            // acquire message
    while (c->flag[0] == 0) {
        std::this_thread::yield();
    }
    _mm_clflushopt(c->x);                     // flushopt x for the reader
    _mm_mfence();                             // acquire x
    g_go.store(1, std::memory_order_release); // inform the reader
}

static int machine1(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    std::thread flusher(flusher_body, c);
    std::thread reader([c] {
        while (g_go.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        volatile char v = c->x[0]; // read x
        (void)v;
    });
    flusher.join();
    reader.join();
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

    pid_t child = csan_test_spawn_machine(kHosts, 1, &machine1, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
