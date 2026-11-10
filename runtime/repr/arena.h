// SPDX-License-Identifier: MIT
// Arena: bump allocator with per-class free lists, in the shared segment.
// Blocks are byte offsets, not pointers: processes map the segment at
// different addresses. Offset 0 is null and never handed out.
#ifndef CSAN_ARENA_H
#define CSAN_ARENA_H

#include "state/layout.h"

#include <cstdint>

namespace csan {

inline uint64_t arena_class_size(uint32_t cls) {
    switch (cls) {
    case kClassSlotMap:
        // + SlotProv under tracing
        return align_up(sizeof(SlotMap), 8) + (g_prov ? sizeof(SlotProv) : 0);
    case kClassSplit:
        // + one last-write Pos per byte under tracing
        return align_up(8 * sizeof(ByteCell), 8) + (g_prov ? 8 * sizeof(uint32_t) : 0);
    case kClassVC:
        return align_up(static_cast<uint64_t>(g_threads) * sizeof(uint32_t), 8);
    case kClassFullMap:
        // states, E clocks, E_r clocks; under tracing, record and finish Pos
        return align_up(align_up(g_threads, 8) + static_cast<uint64_t>(g_threads) * 8, 8) +
               (g_prov ? static_cast<uint64_t>(g_threads) * 8 : 0);
    default:
        return align_up(sizeof(ReadList), 8);
    }
}

inline void* arena_at(uint32_t off) {
    return g_arena + off;
}

// A zeroed block of `cls`, or 0 if the arena is exhausted (reported once).
uint32_t arena_alloc(uint32_t cls);

// Passing 0 is a no-op.
void arena_free(uint32_t cls, uint32_t off);

} // namespace csan

#endif
