// SPDX-License-Identifier: MIT
// Per-thread event rings with call stacks (CSAN_TRACE), for TSan-style reports.
#ifndef CSAN_TRACE_H
#define CSAN_TRACE_H

#include "state/layout.h"

#include <cstdio>

namespace csan {

inline bool tracing() {
    return g_trace_depth != 0;
}

// Record an event; returns its EventRef, or 0 when tracing is off.
// noinline/always_inline fix the frame depth so frame 0 is the hook's caller.
__attribute__((noinline)) uint32_t trace_record_event(uint8_t kind, uintptr_t addr, uint32_t len,
                                                      uint32_t tid, uint32_t from, uint32_t src);

[[gnu::always_inline]] inline uint32_t trace_record(uint8_t kind, uintptr_t addr, uint32_t len,
                                                    uint32_t tid, uint32_t from = 0,
                                                    uint32_t src = 0) {
    return tracing() ? trace_record_event(kind, addr, len, tid, from, src) : 0;
}

// Pos of this thread's last event, credited to state updates in the same hook.
uint32_t trace_cur_pos();

// The Pos to credit a state update with: trace_cur_pos(), or 0 when provenance
// is off. The one place rules ask for a position.
inline uint32_t trace_pos() {
    return g_prov ? trace_cur_pos() : 0;
}

// A flush's verdict is known only after its rule runs, after it was recorded.
void trace_set_from(uint32_t ref, uint32_t from);

// Null when tracing is off, `ref` is 0, or the ring slot was reused.
const Event* trace_event_at(uint32_t ref);

// Thread `tid`'s latest event of `kind` covering `addr` (0 = any), or null.
const Event* trace_find_back(uint32_t tid, uint8_t kind, uintptr_t addr, uint32_t len);

// Like trace_find_back, searching forward from `after_seq`.
const Event* trace_find_after(uint32_t tid, uint32_t after_seq, uint8_t kind, uintptr_t addr,
                              uint32_t len = 1);

const char* event_kind_name(uint8_t kind);

// "clwb", "clflush", ... for a write-back or flush; generic if unrecorded.
const char* event_src_name(uint16_t src, bool is_wb);

// Print a TSan-style finding, one stack per link of the chain. No-op when off.
// `byte` is the byte whose state decided it: `addr`, or for a line kind the
// byte left on the line.
void trace_print_finding(std::FILE* out, uint32_t race_kind, uintptr_t addr, uint32_t len,
                         uint32_t tid, uint32_t other_tid, uintptr_t byte);

// "#N func file:line (mod+0xoff)" per frame, innermost first.
void trace_print_stack(std::FILE* out, uint32_t stack_id);

} // namespace csan

#endif
