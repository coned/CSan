// SPDX-License-Identifier: MIT
// Creating, attaching to, and configuring the shared segment. The first
// process initializes it; others recompute the Layout and attach.
#ifndef CSAN_SEGMENT_H
#define CSAN_SEGMENT_H

#include "state/layout.h"

#include <cstdint>

namespace csan {

// Parse a size with an optional K/M/G suffix. False if the text is not one.
bool parse_size(const char* s, uint64_t* out);

// Tunables, read once from the environment so every process's Layout agrees.
uint32_t configured_clock_entries(); // CSAN_CLOCK_ENTRIES
uint32_t configured_threads();       // CSAN_THREADS
uint64_t configured_arena(uint64_t cxl_size); // CSAN_ARENA
uint32_t configured_trace_depth(void);        // CSAN_TRACE, CSAN_TRACE_DEPTH
uint32_t configured_trace_stacks(void);       // CSAN_TRACE_STACKS

// Next free thread id, or UINT32_MAX (reported) when the clock width is full.
// Caller holds the global lock.
uint32_t allocate_tid_locked();

// Map the segment and set the globals, initializing it if first. Aborts on failure.
void attach_shared_segment(uint32_t hosts, uint64_t cxl_size);

// Once per process: resolve config (arguments, then environment, then setter
// hooks), attach, register the machine and main thread, install the exit hook.
void csan_ensure_init(uint32_t hosts, uint32_t machine, uint64_t cxl_size);

} // namespace csan

#endif
