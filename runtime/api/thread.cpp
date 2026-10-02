// SPDX-License-Identifier: MIT
// Thread creation and joining.
#include "csan_runtime.h"
#include "base/atomics.h"
#include "state/layout.h"
#include "rules/rules.h"
#include "state/segment.h"
#include "base/spinlock.h"
#include "state/state.h"

#include <cerrno>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

using namespace csan;

// ---- process creation ------------------------------------------------------
//
// fork() is interposed so the Fork edge is taken from the parent's clock as of
// the fork, not later. The forking thread copies its clock into private memory just before the real
// fork, so the child inherits the copy, then ticks in the parent. csan_spawn
// (api_lifecycle.cpp) merges the copy.

extern "C" pid_t fork(void) {
    static pid_t (*real_fork)(void) =
        reinterpret_cast<pid_t (*)(void)>(dlsym(RTLD_NEXT, "fork"));
    if (real_fork == nullptr) {
        errno = ENOSYS;
        return -1;
    }
    if (!g_attached) {
        return real_fork();
    }
    uint32_t t = __csan_thread_id();
    if (t != UINT32_MAX) {
        t_fork_clock.assign(g_threads, 0);
        VectorClock(t_fork_clock.data()).copy_from(thread_vc(t));
        t_fork_parent = t;
    }
    pid_t pid = real_fork();
    if (pid > 0 && t != UINT32_MAX) {
        t_fork_clock.clear();
        t_fork_parent = UINT32_MAX;
        tick(t);
    }
    return pid;
}

// ---- thread creation -------------------------------------------------------
//
// The parent's clock must reach the child BEFORE it runs any user code; after
// pthread_create returns is too late, since the child may already be running.
//
// pthread_create is interposed. A strong definition here overrides libc's for
// the whole program, libstdc++'s std::thread included, so every thread is
// covered without the pass recognizing how it was created. The child's thread
// id and clock are assigned in the PARENT; the trampoline adopts them.

namespace {

struct SpawnState {
    void* (*start)(void*);
    void* arg;
    uint32_t tid; // pre-assigned by the parent, UINT32_MAX if none was free
};

void* csan_thread_trampoline(void* p) {
    SpawnState* st = static_cast<SpawnState*>(p);
    void* (*start)(void*) = st->start;
    void* arg = st->arg;
    uint32_t tid = st->tid;
    delete st;

    if (tid != UINT32_MAX && g_attached) {
        t_tid = tid;
        spin_lock(&g_header->global_lock);
        // Record the mapping here rather than in the parent: the parent cannot
        // do it until pthread_create returns, by which time this thread may
        // already have run and exited.
        thread_ids()[pthread_self()] = tid;
        spin_unlock(&g_header->global_lock);
    }
    return start(arg);
}

} // namespace

extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                              void* (*start)(void*), void* arg) {
    static int (*real_create)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) =
        reinterpret_cast<int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)>(
            dlsym(RTLD_NEXT, "pthread_create"));
    if (real_create == nullptr) {
        return EAGAIN;
    }
    if (!g_attached) {
        return real_create(thread, attr, start, arg);
    }

    uint32_t parent = __csan_thread_id();
    SpawnState* st = new SpawnState{start, arg, UINT32_MAX};

    spin_lock(&g_header->global_lock);
    uint32_t child = allocate_tid_locked();
    if (child != UINT32_MAX) {
        atomic_store_u32(&g_header->machine_of_thread[child], g_machine);
        rule_fork_locked(parent, child); // the fork edge, established up front
    }
    st->tid = child;
    spin_unlock(&g_header->global_lock);

    int rc = real_create(thread, attr, csan_thread_trampoline, st);
    if (rc != 0) {
        delete st;
    }
    return rc;
}

void __csan_pthread_join(uint64_t pth_value) {
    if (!g_attached) {
        return;
    }
    uint32_t tid = __csan_thread_id();
    if (tid == UINT32_MAX) {
        return;
    }
    pthread_t pth = static_cast<pthread_t>(pth_value);
    spin_lock(&g_header->global_lock);
    auto it = thread_ids().find(pth);
    if (it != thread_ids().end() && it->second < g_threads) {
        rule_join_locked(tid, it->second);
    }
    spin_unlock(&g_header->global_lock);
}
