// SPDX-License-Identifier: MIT
// Emitting findings, and the address-range trace used to debug them.
#ifndef CSAN_REPORT_H
#define CSAN_REPORT_H

#include "repr/epoch.h"
#include "state/layout.h"

#include <cstdint>

namespace csan {

// CSAN_TRACE_RANGE="<lo>:<hi>" (pool offsets, hex accepted) logs every read,
// write, release, acquire, write-back and flush the checker sees inside that
// range, with the acting thread's clock. A write-back or flush line also says
// whether it recorded anything, which is how a flush that acquires nothing is
// told from one that never ran. Set by csan_ensure_init.
extern uint64_t g_trace_lo;
extern uint64_t g_trace_hi;
extern bool g_trace_on;

// How many findings a run prints before it goes quiet. CSAN_REPORT_LIMIT sets
// it; 0 means no limit. Counting is unaffected, so csan_race_events() and the
// per-kind totals stay exact after the limit is hit.
extern uint64_t g_report_limit;

// Is this address inside the traced range?
bool trace_hit(uintptr_t addr);

// One trace line. `what` names the event, `note` is free-form context.
void trace_event(const char* what, uintptr_t addr, uint32_t tid, const char* note);

// The name of a finding kind (CSAN_RACE_* in csan_runtime.h): the string that
// appears in its CSAN RACE line. "?" for a kind out of range.
const char* race_kind_name(uint32_t kind);

// Emit a finding: appends to the shared ring the report API reads, writes it
// to stderr, and counts it -- both in the total and under its own kind, which
// is what a test asserts on. `other` is the epoch the access conflicted with,
// where there is one.
//
// Takes the global lock, and that lock is NOT recursive: never call this while
// holding it.
void report_race(uint32_t kind, uintptr_t addr, uint32_t tid, Epoch other = kEpochBot);

} // namespace csan

#endif
