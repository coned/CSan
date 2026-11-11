// SPDX-License-Identifier: MIT
// Process-local runtime state (segment-backed state is in layout.h).
#ifndef CSAN_STATE_H
#define CSAN_STATE_H

#include "repr/epoch.h"
#include "state/layout.h"

#include <cstdint>
#include <mutex>
#include <pthread.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace csan {

// Which of t's entries a fence of t must visit, and what it applies:
//   kPendingPub       P_x[t] or P'_x[t] = Pending   SFence, MFence: ApplyFin
//   kPendingAcq       A_x[t] = Pending   SFence: ApplySFin   MFence: ApplyMFin
//   kPendingAcqAllowStore   A_x[t] = AllowStore   MFence: ApplyMFin
// The rules apply these functions to every byte; an entry they leave
// unchanged needs no visit, so only these are queued.
enum PendingKind {
    kPendingPub = 0,
    kPendingAcq = 1,
    kPendingAcqAllowStore = 2,
};

// A write-back or flush record awaiting the issuing thread's next fence.
struct Pending {
    uintptr_t addr; // the byte or line
    Epoch epoch;    // its tid keys the slot to complete
};

// Process-local: a fence only completes its own thread's records.
std::vector<Pending>& pending_queue(uint32_t t, uint32_t kind);

// UINT32_MAX until assigned.
extern __thread uint32_t t_tid __attribute__((tls_model("initial-exec"))); // __thread: no TLS init wrapper on the hook path

// Nonzero inside a hook that holds the global lock across the program's own
// access (a releasing CAS). A SIGSEGV handler run there would deadlock on that
// lock, so its accesses go unchecked.
extern __thread uint32_t t_hook_depth __attribute__((tls_model("initial-exec")));

inline bool inside_hook() {
    return t_hook_depth != 0;
}

// Call release() as soon as the lock is dropped.
class HookDepth {
public:
    HookDepth() { ++t_hook_depth; }
    ~HookDepth() { release(); }
    HookDepth(const HookDepth&) = delete;
    HookDepth& operator=(const HookDepth&) = delete;
    void release() {
        if (held_) {
            held_ = false;
            --t_hook_depth;
        }
    }

private:
    bool held_ = true;
};

// pthread_t -> tid, for join. Written by the child in the create trampoline.
std::unordered_map<pthread_t, uint32_t>& thread_ids();

// The forking thread's clock, copied just before fork() so the child's Fork
// edge uses the clock at fork time, not the parent's advancing row. Empty when
// no fork is pending.
extern thread_local std::vector<uint32_t> t_fork_clock;
extern thread_local uint32_t t_fork_parent;

// Segment name, and whether this process created it (and must unlink it).
std::string& shm_name();
extern bool g_initializer;

// Set by the setter hooks before init.
extern uint32_t g_pending_hosts;
extern uint64_t g_pending_cxl_size;

// One-time init; the buffer __csan_report() returns.
extern std::once_flag g_init_once;
std::string& report_buffer();

} // namespace csan

#endif
