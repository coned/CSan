// SPDX-License-Identifier: MIT
// SpinLock: a mutex usable from any process mapping the segment.
#ifndef CSAN_SPINLOCK_H
#define CSAN_SPINLOCK_H

#include "base/atomics.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

namespace csan {

// Zero-initialized = free, so a zeroed segment needs no per-process setup.
struct SpinLock {
    uint32_t state; // 0 = free, else the holder's pid
};

// Cached pid (the lock is hot). Cleared in a forked child, which must not lock
// under its parent's pid.
inline uint32_t g_lock_pid = 0;

inline uint32_t lock_self_pid() {
    uint32_t pid = __atomic_load_n(&g_lock_pid, __ATOMIC_RELAXED);
    if (pid == 0) {
        static int registered = pthread_atfork(
            nullptr, nullptr, [] { __atomic_store_n(&g_lock_pid, 0u, __ATOMIC_RELAXED); });
        (void)registered;
        pid = static_cast<uint32_t>(getpid());
        __atomic_store_n(&g_lock_pid, pid, __ATOMIC_RELAXED);
    }
    return pid;
}

// Whether `pid` has exited. A zombie still answers kill(), so check /proc too.
inline bool lock_owner_dead(uint32_t pid) {
    if (kill(static_cast<pid_t>(pid), 0) != 0) {
        return errno == ESRCH;
    }
    char path[32];
    snprintf(path, sizeof(path), "/proc/%u/stat", pid);
    FILE* f = fopen(path, "r");
    if (f == nullptr) {
        return false;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char* paren = strrchr(buf, ')');
    return paren != nullptr && (paren[1] == ' ') && (paren[2] == 'Z' || paren[2] == 'X');
}

// Short critical sections, so pause/yield backoff suffices. A long waiter takes
// over a lock whose holder died; the state it was updating is not repaired.
inline void spin_lock_u32(uint32_t* state) {
    const uint32_t self = lock_self_pid();
    uint32_t spins = 0;
    uint32_t yields = 0;
    for (;;) {
        uint32_t owner = 0;
        if (__atomic_compare_exchange_n(state, &owner, self, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        ++spins;
        if (spins < 64) {
            __builtin_ia32_pause();
            continue;
        }
        sched_yield();
        spins = 0;
        if (++yields % 1024 == 0 && owner != self && lock_owner_dead(owner) &&
            __atomic_compare_exchange_n(state, &owner, self, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            fprintf(stderr, "CSAN: process %u died holding a runtime lock; taken over\n", owner);
            return;
        }
    }
}

inline void spin_unlock_u32(uint32_t* state) {
    __atomic_store_n(state, 0, __ATOMIC_RELEASE);
}

inline void spin_lock(SpinLock* l) {
    spin_lock_u32(&l->state);
}

inline void spin_unlock(SpinLock* l) {
    spin_unlock_u32(&l->state);
}

} // namespace csan

#endif
