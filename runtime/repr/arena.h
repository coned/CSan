// SPDX-License-Identifier: MIT
// The arena: a bump-and-free-list allocator for the variable-size parts of the
// shadow state, inside the shared segment.
//
// Blocks are addressed by byte offset from the arena base, never by pointer:
// processes map the segment at different addresses. Offset 0 is null, so the
// arena's first eight bytes are never handed out. One free list per size
// class (ArenaClass in layout.h).
#ifndef CSAN_ARENA_H
#define CSAN_ARENA_H

#include "state/layout.h"

#include <cstdint>

namespace csan {


inline uint64_t arena_class_size(uint32_t cls) {
    switch (cls) {
    case kClassSlotMap:
        // + SlotProv under tracing: which code recorded, and finished, each slot
        return align_up(sizeof(SlotMap), 8) + (g_prov ? sizeof(SlotProv) : 0);
    case kClassSplit:
        // + one last-write Pos per byte under tracing
        return align_up(8 * sizeof(ByteCell), 8) + (g_prov ? 8 * sizeof(uint32_t) : 0);
    case kClassVC:
        return align_up(static_cast<uint64_t>(g_threads) * sizeof(uint32_t), 8);
    case kClassFullMap:
        // promoted SlotMap: `threads` states then `threads` clocks, then under
        // tracing `threads` record Pos and `threads` finish Pos
        return align_up(align_up(g_threads, 8) + static_cast<uint64_t>(g_threads) * 4, 8) +
               (g_prov ? static_cast<uint64_t>(g_threads) * 8 : 0);
    default:
        return align_up(sizeof(ReadList), 8);
    }
}

inline void* arena_at(uint32_t off) {
    return g_arena + off;
}

// Returns a zeroed block of `cls`, or 0 when the arena is exhausted (reported
// once: unrecorded state can cause false reports).
uint32_t arena_alloc(uint32_t cls);

// Returns a block to its class's free list. Passing 0 is a no-op.
void arena_free(uint32_t cls, uint32_t off);

} // namespace csan

#endif
