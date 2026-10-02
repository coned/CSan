// SPDX-License-Identifier: MIT
// Recording what each thread did, and the call stack that did it (CSAN_TRACE).
//
// The findings this feeds are shaped like ThreadSanitizer's: a headline naming
// the access, then blocks of numbered frames for the write, the write-back, the
// acquire and the synchronization that was or was not there. So what is stored
// here is events, not state -- a report is a walk over two threads' rings, not
// a dump of the shadow.
#ifndef CSAN_TRACE_H
#define CSAN_TRACE_H

#include "state/layout.h"

#include <cstdio>

namespace csan {

// Is tracing on? One predicted branch; every cost below sits behind it.
inline bool tracing() {
    return g_trace_depth != 0;
}

// Record one event by thread `tid` and return its EventRef, or 0 when tracing
// is off. `from` is the EventRef of the release an acquire merged from, else 0.
//
// Never inlined: the stack is captured here with __builtin_return_address, so
// this function's position in the frame chain has to be fixed. Frame 0 of the
// recorded stack is the caller's caller -- the program's code, not the hook.
__attribute__((noinline)) uint32_t trace_record(uint8_t kind, uintptr_t addr, uint32_t len,
                                                uint32_t tid, uint32_t from = 0,
                                                uint32_t src = 0);

// The Pos (interned stack id) of the event this thread recorded last, which
// every state update made in the same hook is credited to. 0 before any.
uint32_t trace_cur_pos();

// Set an already recorded event's `from`: a flush's verdict is known only
// after the rule has run, and the event is recorded first so the rule can
// credit its state to it.
void trace_set_from(uint32_t ref, uint32_t from);

// The event `ref` names, or null when tracing is off, `ref` is 0, or the ring
// slot has since been reused -- history exhausted, which a report says plainly
// rather than passing off some later event as the one asked for.
const Event* trace_event_at(uint32_t ref);

// Thread `tid`'s most recent event of kind `kind` whose range covers `addr`,
// searching back at most the ring's depth. `addr` of 0 matches any. Null when
// there is none, or none left.
const Event* trace_find_back(uint32_t tid, uint8_t kind, uintptr_t addr, uint32_t len);

// Like trace_find_back, but searching FORWARD from `after` -- which is how a
// write-back is found after its write, and a fence after its write-back.
const Event* trace_find_after(uint32_t tid, uint32_t after_seq, uint8_t kind, uintptr_t addr,
                              uint32_t len = 1);

// The name of an event kind, for a report.
const char* event_kind_name(uint8_t kind);

// The instruction a write-back or flush came from -- "clwb", "clflush",
// "non-temporal store" -- with " (inline asm)" where that is what it was.
// "write-back" / "flush" when the construct was not recorded.
const char* event_src_name(uint16_t src, bool is_wb);

// Print the finding as ThreadSanitizer prints one: a headline, then a block of
// numbered frames for each link -- the access, the write it conflicts with, the
// write-back, the publish, the acquire. A link the history no longer holds says
// so rather than being left out. Does nothing when tracing is off.
void trace_print_finding(std::FILE* out, uint32_t race_kind, uintptr_t addr, uint32_t len,
                         uint32_t tid, uint32_t other_tid);

// Print stack `id` the way TSan prints one: a "#N func file:line (mod+0xoff)"
// line per frame, innermost first, indented by four spaces.
void trace_print_stack(std::FILE* out, uint32_t stack_id);

} // namespace csan

#endif
