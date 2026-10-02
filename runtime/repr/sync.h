// SPDX-License-Identifier: MIT
// The two clock tables an address can carry: 𝕃_a for a lock, 𝕊_x for an
// atomic location.
//
// 𝕊_x is ONE vector clock per location -- the clock at the head of the release
// sequence currently live there. A release store overwrites it, any other
// store clears it, a read-modify-write leaves it alone, and an acquire joins
// it. It is not keyed by machine: an acquire on one machine synchronizes with
// a release on another.
//
// Both tables are keyed by `sync_key` (rules_sync.cpp), not by raw address: a
// pool location is named by its pool offset, since exec'd machines map the
// pool at different addresses, and a private location by address plus machine,
// since forked machines share private addresses.
//
// Every function here requires the caller to hold g_header->global_lock, and
// none of them reports: report_race takes that same non-recursive lock, so a
// caller that needs to report does it after unlocking.
#ifndef CSAN_SYNC_H
#define CSAN_SYNC_H

#include "repr/arena.h"
#include "repr/vectorclock.h"
#include "state/layout.h"

#include <cstdint>

namespace csan {

// ---- 𝕃_a: one vector clock per lock -----------------------------------------

// Entry i of a table of ClockEntry, walked with the run-time stride, and the
// clock that follows its header.
ClockEntry* clock_entry(uint8_t* table, uint32_t i);
VectorClock clock_entry_vc(ClockEntry* e);

// Existing entry for `key`, or null. find_or_add takes a fresh slot when there
// is none and returns null, loudly, if the table is full: a lock at a new
// address then gets no clock, and every edge through it is lost.
ClockEntry* find_clock_entry(uint8_t* table, uint32_t count, uint64_t key);
ClockEntry* find_or_add_clock_entry(uint8_t* table, uint32_t* count, uint64_t key);

// ---- 𝕊_x: the release-sequence clock of an atomic location -------------------

// 𝕊_x, read-only: the zero clock when this location has none.
VectorClock sync_s(uint64_t key);

// 𝕊_x ← c, allocating the entry and its clock on first use. Silently does
// nothing when the table or the arena is exhausted, which is counted and
// reported by the allocator.
void sync_s_set(uint64_t key, const VectorClock& c);

// 𝕊_x ← 𝕊_x ⊔ c, allocating on first use: a releasing read-modify-write adds
// its own clock to the sequence it continues.
void sync_s_merge(uint64_t key, const VectorClock& c);

// 𝕊_x ← 0, releasing the entry: the release sequence at this location is over.
void sync_s_clear(uint64_t key);

// 𝕊_x ← 0 for every key in [begin, end). A plain write ends the release
// sequence at every byte it covers. Takes the global lock itself, so the
// caller must NOT hold it; the range is of pool offsets.
void sync_clear_range(uint64_t begin, uint64_t end);

} // namespace csan

#endif
