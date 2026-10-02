// SPDX-License-Identifier: MIT
// Process-local state: what this process knows that is not in the shared
// segment, and so is not shared with the other machines.
//
// The segment-backed pointers and counters are in layout.h. Everything here is
// per process and rebuilt from scratch after a fork.
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

// Which relay a queued record belongs to, so a fence knows what to complete.
enum PendingKind {
    kPendingPubByte = 0, // an entry in a byte's publication map
    kPendingPubLine = 1, // an entry in a line's publication map
    kPendingAcqByte = 2, // an entry in a byte's acquisition map
    kPendingAcqLine = 3, // an entry in a line's acquisition map
};

// One record a write-back or flush left for the next fence to complete.
//
//   off  size  field  contents
//   ---  ----  -----  -------------------------------------------------------
//     0     8  addr   the byte, or the line, the record sits on
//     8     4  epoch  the epoch recorded; its tid is the key of the slot to
//                     complete
struct Pending {
    uintptr_t addr;
    Epoch epoch;
};

// Per-thread pending queues, one per relay kind, indexed by thread id. Process
// local rather than shared: a queue belongs to the thread that issued the clwb
// or clflushopt, and a fence only ever completes its own thread's records.
std::vector<Pending>& pending_queue(uint32_t t, uint32_t kind);

// This thread's id, or UINT32_MAX before it has been assigned one.
extern thread_local uint32_t t_tid;

// Whether this thread is inside a hook that holds the global lock ACROSS the
// program's own access -- only a releasing compare-exchange (rules_sync.cpp).
// A program that demand-maps its CXL memory faults inside that access and runs
// its SIGSEGV handler on this thread; the handler's instrumented accesses would
// wait for the lock this thread holds, so they are performed unchecked instead.
extern thread_local uint32_t t_hook_depth;

inline bool inside_hook() {
    return t_hook_depth != 0;
}

// Raises t_hook_depth for the critical section it guards; release() lowers it
// as soon as the lock is dropped, and the destructor covers an early return.
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

// pthread_t -> thread id, so a join can find the clock of the thread it
// joined. Written by the pthread_create trampoline from inside the child.
std::unordered_map<pthread_t, uint32_t>& thread_ids();

// The forking thread's clock, copied into private memory just before fork()
// (thread.cpp), so the child process can take its Fork edge from the clock AS
// OF THE FORK rather than from the parent's live row, which the parent goes on
// advancing. Thread-local: the child's main thread IS the forking thread, so it
// inherits exactly this copy. Empty when no fork is pending.
extern thread_local std::vector<uint32_t> t_fork_clock;
extern thread_local uint32_t t_fork_parent;

// Segment name, and whether THIS process created it and so must unlink it.
std::string& shm_name();
extern bool g_initializer;

// Configuration a program supplied through the setter hooks before init, for
// csan_ensure_init to use when it runs.
extern uint32_t g_pending_hosts;
extern uint64_t g_pending_cxl_size;

// Guards the one-time init, and the buffer __csan_report() returns.
extern std::once_flag g_init_once;
std::string& report_buffer();

} // namespace csan

#endif
