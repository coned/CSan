// SPDX-License-Identifier: MIT
// Creating, attaching to, and configuring the shared segment.
//
// The first process to arrive sizes and initializes the mapping; every other
// process recomputes the same Layout from the same environment and attaches to
// it, refusing if the header disagrees. Configuration comes from the
// environment or from the setter hooks a program calls before init.
#ifndef CSAN_SEGMENT_H
#define CSAN_SEGMENT_H

#include "state/layout.h"

#include <cstdint>

namespace csan {

// Parse a size with an optional K/M/G suffix. False if the text is not one.
bool parse_size(const char* s, uint64_t* out);

// The tunables, each read once from the environment so that every process
// computing a Layout agrees. Defaults are in layout.h.
uint32_t configured_clock_entries(); // CSAN_CLOCK_ENTRIES
uint32_t configured_threads();       // CSAN_THREADS
uint64_t configured_arena(uint64_t cxl_size); // CSAN_ARENA
uint32_t configured_trace_depth(void);        // CSAN_TRACE, CSAN_TRACE_DEPTH
uint32_t configured_trace_stacks(void);       // CSAN_TRACE_STACKS

// Take the next free thread id. Returns UINT32_MAX, loudly, when the segment
// has no slot left: a thread past the clock width has nowhere to record its
// clock, so every edge through it would be lost.
//
// Caller must hold the global lock.
uint32_t allocate_tid_locked();

// Map the segment, pointing the globals at its areas, initializing it if this
// process got there first. Aborts if it cannot.
void attach_shared_segment(uint32_t hosts, uint64_t cxl_size);

// Attach once per process, resolving hosts, machine id and pool size from the
// arguments, then the environment, then whatever the setter hooks recorded.
// Registers this process's machine, takes a thread id for its main thread, and
// installs the exit hook that prints the stats line.
void csan_ensure_init(uint32_t hosts, uint32_t machine, uint64_t cxl_size);

} // namespace csan

#endif
