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

// 4 bytes.
//
//   off  size  field  contents
//   ---  ----  -----  -------------------------------------------------------
//     0     4  state  0 = free, else the pid of the process holding it
//
// No constructor and no POSIX object to initialize per process: a zeroed
// segment is a segment full of free locks.
struct SpinLock {
    uint32_t state;
};

// This process's pid, cached: getpid() is a system call and the lock is hot.
// Cleared in a forked child, which must not take the lock under its parent's pid.
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

// Whether `pid` has exited. A zombie still answers kill(), so its state in
// /proc is consulted too: a parent that has not reaped it yet is common.
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

// Process-shared spin lock: works on any MAP_SHARED memory (no constructor, no
// POSIX object to initialize in every process). Critical sections are short,
// so a pause/yield backoff is all that is needed.
//
// A process killed inside a critical section never unlocks. A waiter that has
// yielded long enough checks whether the holder is still alive and, if it is
// not, takes the lock over. What the dead process was in the middle of
// updating is not repaired; the survivors go on with it as it was left.
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
