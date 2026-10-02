// SPDX-License-Identifier: MIT
// Two machines write the same byte of one line under a shared pthread mutex, with the write-back chain.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>
#include <pthread.h>

namespace {
constexpr uint32_t kHosts = 2;

struct Shared {
    pthread_mutex_t m;
    char pad[64 - sizeof(pthread_mutex_t) % 64];
    char x[64];
};

Shared* region() {
    return static_cast<Shared*>(csan_shared_region(1, sizeof(Shared)));
}

int writer2(void*) {
    Shared* s = region();
    pthread_mutex_lock(&s->m);
    _mm_clflushopt(s->x);
    _mm_mfence();
    s->x[0] = 2;
    _mm_clwb(s->x);
    _mm_sfence();
    pthread_mutex_unlock(&s->m);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Shared* s = region();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->m, &attr);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer2, nullptr);
    pthread_mutex_lock(&s->m);
    _mm_clflushopt(s->x);
    _mm_mfence();
    s->x[0] = 1;
    _mm_clwb(s->x);
    _mm_sfence();
    pthread_mutex_unlock(&s->m);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
