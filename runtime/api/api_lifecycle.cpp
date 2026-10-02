// SPDX-License-Identifier: MIT
// Process and thread lifecycle: attaching, binding to a machine, thread ids.
#include "csan_runtime.h"
#include "base/atomics.h"
#include "state/layout.h"
#include "rules/rules.h"
#include "state/segment.h"
#include "base/spinlock.h"
#include "state/state.h"
#include "repr/vectorclock.h"

#include <cstdio>
#include <pthread.h>
#include <unistd.h>

using namespace csan;

extern "C" {

void csan_init(uint32_t hosts, uint32_t machine, uint64_t cxl_mem_size) {
    csan_ensure_init(hosts, machine, cxl_mem_size);
}

void csan_spawn(uint32_t hosts, uint32_t machine) {
    // This process is a forked child. The interposed fork() left the forking
    // thread's clock as of the fork in t_fork_clock (thread.cpp); that is the
    // Fork edge. Without it (a child not created through our fork), the
    // inherited t_tid's live clock is used.
    uint32_t parent = t_fork_parent != UINT32_MAX ? t_fork_parent : t_tid;
    std::vector<uint32_t> parent_clock = std::move(t_fork_clock);
    t_fork_clock.clear();
    t_fork_parent = UINT32_MAX;
    t_tid = UINT32_MAX;
    // glibc can reuse a dead thread's pthread_t after fork, which would alias
    // a new thread to an inherited tid; this machine's threads get fresh ones.
    thread_ids().clear();
    g_initializer = false; // never unlink the segment from a child
    if (!g_attached) {
        csan_ensure_init(hosts, machine, 0);
        return;
    }
    if (hosts > 0 && hosts <= kMaxMachines) {
        atomic_store_u32(&g_header->hosts, hosts);
    }
    if (machine >= kMaxMachines) {
        machine = 0;
    }
    spin_lock(&g_header->global_lock);
    g_header->machine_pid[machine] = static_cast<uint32_t>(getpid());
    g_header->machine_registered[machine] = 1;
    g_machine = machine;
    uint32_t tid = allocate_tid_locked();
    if (tid != UINT32_MAX) {
        atomic_store_u32(&g_header->machine_of_thread[tid], machine);
        g_header->machine_main_tid[machine] = tid;
        if (!parent_clock.empty()) {
            rule_fork_process_locked(VectorClock(parent_clock.data()), tid);
        } else {
            rule_fork_locked(parent < g_threads ? parent : UINT32_MAX, tid);
        }
    }
    spin_unlock(&g_header->global_lock);
    if (tid != UINT32_MAX) {
        t_tid = tid;
    }
}

// Join with the machine process `pid`, which the caller has waited for: the
// Join rule from that machine's main thread, exactly as pthread_join is a Join
// from the joined thread.
void csan_join(uint32_t pid) {
    uint32_t t = __csan_thread_id();
    if (t == UINT32_MAX) {
        return;
    }
    spin_lock(&g_header->global_lock);
    for (uint32_t m = 0; m < kMaxMachines; ++m) {
        if (g_header->machine_registered[m] != 0 && g_header->machine_pid[m] == pid) {
            uint32_t u = g_header->machine_main_tid[m];
            if (u < g_threads && u != t) {
                rule_join_locked(t, u);
            }
            break;
        }
    }
    spin_unlock(&g_header->global_lock);
}

uint32_t csan_thread_id(void) {
    return __csan_thread_id();
}

uint32_t csan_machine_id(void) {
    return __csan_machine_id();
}

void __csan_init(void) {
    if (g_attached) {
        return;
    }
    // A program that must discover its CXL region first (an allocator that
    // maps at a fixed address, say) calls csan_init itself at the right
    // moment and asks the pass-inserted call to stand aside.
    const char* defer = getenv("CSAN_DEFER_INIT");
    if (defer != nullptr && *defer != '\0' && *defer != '0') {
        return;
    }
    const char* name = getenv("CSAN_SHM_NAME");
    if (name == nullptr || *name == '\0') {
        return;
    }
    csan_ensure_init(0, UINT32_MAX, 0);
}

uint32_t __csan_thread_id(void) {
    if (t_tid != UINT32_MAX) {
        return t_tid;
    }
    if (!g_attached) {
        return UINT32_MAX;
    }
    spin_lock(&g_header->global_lock);
    pthread_t self = pthread_self();
    auto it = thread_ids().find(self);
    uint32_t tid = UINT32_MAX;
    if (it != thread_ids().end()) {
        tid = it->second;
    } else {
        tid = allocate_tid_locked();
        if (tid != UINT32_MAX) {
            thread_ids()[self] = tid;
            atomic_store_u32(&g_header->machine_of_thread[tid], g_machine);
            thread_vc(tid).set(tid, 1);
        }
    }
    spin_unlock(&g_header->global_lock);
    if (tid != UINT32_MAX) {
        t_tid = tid;
    }
    return t_tid;
}

void __csan_set_thread_id(uint32_t tid) {
    if (tid >= kMaxThreads) {
        return;
    }
    t_tid = tid;
    if (!g_attached) {
        return;
    }
    spin_lock(&g_header->global_lock);
    atomic_store_u32(&g_header->machine_of_thread[tid], g_machine);
    if (vc_get(tid, tid) == 0) {
        thread_vc(tid).set(tid, 1);
    }
    spin_unlock(&g_header->global_lock);
}

uint32_t __csan_machine_id(void) {
    if (!g_attached) {
        return UINT32_MAX;
    }
    return g_machine;
}

void __csan_set_machine(uint32_t mid) {
    if (mid >= kMaxMachines) {
        return;
    }
    g_machine = mid;
    if (!g_attached) {
        return;
    }
    uint32_t tid = t_tid;
    spin_lock(&g_header->global_lock);
    g_header->machine_pid[mid] = static_cast<uint32_t>(getpid());
    g_header->machine_registered[mid] = 1;
    if (tid != UINT32_MAX) {
        atomic_store_u32(&g_header->machine_of_thread[tid], mid);
    }
    spin_unlock(&g_header->global_lock);
}

void __csan_set_hosts(uint32_t hosts) {
    if (hosts == 0 || hosts > kMaxMachines) {
        return;
    }
    g_pending_hosts = hosts;
    if (g_attached) {
        atomic_store_u32(&g_header->hosts, hosts);
    }
}

void __csan_set_cxl_size(uint64_t size) {
    if (size == 0) {
        return;
    }
    g_pending_cxl_size = size;
    if (!g_attached) {
        csan_ensure_init(0, UINT32_MAX, size);
    }
}

} // extern "C"
