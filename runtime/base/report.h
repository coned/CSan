// SPDX-License-Identifier: MIT
// Emitting findings, and the address-range trace used to debug them.
#ifndef CSAN_REPORT_H
#define CSAN_REPORT_H

#include "repr/epoch.h"
#include "state/layout.h"

#include <cstdint>

namespace csan {

// CSAN_TRACE_RANGE="<lo>:<hi>" (pool offsets): log every event in the range
// with the acting thread's clock. Set by csan_ensure_init.
extern uint64_t g_trace_lo;
extern uint64_t g_trace_hi;
extern bool g_trace_on;

// CSAN_REPORT_LIMIT: findings printed per run, 0 = unlimited. Counts stay exact.
extern uint64_t g_report_limit;
// CSAN_REPORT_LIMIT=off: print nothing and take no lock; only the counts move.
extern bool g_report_off;
// CSAN_REPORT_ONCE (default on; 0 turns it off): after a finding at a byte,
// skip that byte's checks, as TSan stops checking a racy word.
extern bool g_report_once;

bool trace_in_range(uintptr_t addr);

inline bool trace_hit(uintptr_t addr) {
    return g_trace_on && trace_in_range(addr);
}

void trace_event(const char* what, uintptr_t addr, uint32_t tid, const char* note);

// Log an event at `addr` if CSAN_TRACE_RANGE covers it.
inline void trace_access(const char* what, uintptr_t addr, uint32_t tid, const char* note = "") {
    if (trace_hit(addr)) {
        trace_event(what, addr, tid, note);
    }
}

// Name of a CSAN_RACE_* kind; "?" if out of range.
const char* race_kind_name(uint32_t kind);

// Record, print and count a finding. `other` is the conflicting epoch, if any;
// `other_addr` the byte it wrote, when that is not `addr` (a line kind: the
// byte left on the line). Takes the global lock (not recursive): caller must
// not hold it.
void report_race(uint32_t kind, uintptr_t addr, uint32_t tid, Epoch other = kEpochBot,
                 uintptr_t other_addr = 0);

} // namespace csan

#endif
