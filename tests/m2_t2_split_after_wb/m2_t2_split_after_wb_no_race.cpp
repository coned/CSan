// SPDX-License-Identifier: MIT
// A partial read between clwb and sfence splits the word; the sfence must publish all eight bytes.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdint>
#include <cstdio>
#include <immintrin.h>

struct Ctx {
    char* x;
};

static int consumer(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->x);
    _mm_mfence();
    volatile char v = c->x[7]; // a byte the producer's partial read did not touch
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    *reinterpret_cast<volatile uint64_t*>(ctx.x) = 0x0102030405060708ull; // the whole word
    _mm_clwb(ctx.x);
    volatile uint32_t lo = *reinterpret_cast<volatile uint32_t*>(ctx.x); // splits the word
    (void)lo;
    _mm_sfence(); // must publish all eight bytes

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(false);
}
