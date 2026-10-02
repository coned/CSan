// SPDX-License-Identifier: MIT
// Machine 1 is exec'd (different pool address); a process-shared mutex in the pool orders the handoff.
// Expected: no race (locks are keyed by pool offset).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstdlib>
#include <immintrin.h>
#include <pthread.h>
#include <unistd.h>

namespace {
constexpr uint32_t kHosts = 2;
constexpr uint64_t kPool = 1u << 16;

struct Shared {
    pthread_mutex_t m; // PTHREAD_PROCESS_SHARED, in the pool
    char x[64];
};

Shared* region() {
    return static_cast<Shared*>(csan_shared_region(1, sizeof(Shared)));
}

int machine1() {
    Shared* s = region();
    pthread_mutex_lock(&s->m);
    _mm_clflushopt(s->x);
    _mm_mfence();
    volatile char v = s->x[0];
    pthread_mutex_unlock(&s->m);
    if (v != 42) {
        std::fprintf(stderr, "FAIL machine 1 read %d\n", v);
        return 1;
    }
    return 0;
}
} // namespace

int main() {
    const char* id = getenv("CSAN_MACHINE_ID");
    if (id != nullptr && *id == '1') {
        return machine1(); // the exec'd process: attached by __csan_init at main
    }

    csan_init(kHosts, 0, kPool);
    Shared* s = region();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->m, &attr);

    pthread_mutex_lock(&s->m);
    s->x[0] = 42;
    _mm_clwb(s->x);
    _mm_sfence();
    pthread_mutex_unlock(&s->m);

    pid_t child = fork();
    if (child == 0) {
        // CSAN_SHM_NAME was exported by csan_init; the child must see the same
        // segment geometry, so the pool size and host count go along.
        setenv("CSAN_MACHINE_ID", "1", 1);
        setenv("CSAN_HOSTS", "2", 1);
        setenv("CSAN_TOTAL_MEM", "65536", 1);
        execl("/proc/self/exe", "m2_t2_exec_machine", static_cast<char*>(nullptr));
        std::perror("execl");
        _exit(127);
    }
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(false);
}
