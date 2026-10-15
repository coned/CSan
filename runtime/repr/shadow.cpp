// SPDX-License-Identifier: MIT
#include "repr/shadow.h"

#include "base/atomics.h"
#include "repr/readclock.h"

#include <cstring>

namespace csan {

// ---- ℝ_x ------------------------------------------------------------------
//
// The reads of the byte since its last write, in FastTrack's adaptive form
// (see ByteCell in layout.h):
//
//   epoch           while the reads are totally ordered by happens-before, the
//                   last read follows all the others, so its epoch c@t alone
//                   decides ℝ_x ⊑ ℂ_u for every later writer u: `rd` holds it
//                   and the check is one covers() -- no indirection, no scan.
//   more readers    after two concurrent reads, the kRdSharedBit is set and
//                   `rd` is the offset of a ReadList (readclock.h) with the
//                   latest read of each thread, scanned in the number of readers.
//
// Losing a reader record to an exhausted arena can only miss a
// write-after-read, never invent one.

// ℝ_x ⊑ ℂ_t
bool rd_le(const ByteCell* c, uint32_t tid) {
    if (!cell_rd_shared(c)) {
        return thread_vc(tid).covers(c->rd);
    }
    return readlist_le(c->rd, tid);
}

// ℝ_x ← addRead(ℝ_x, t), FastTrack's three read cases:
//   epoch ⊑ ℂ_t    the recorded read happens before this one: replace it by
//                  this read's epoch (always so for the same thread);
//   epoch ⋢ ℂ_t    a concurrent read: move both into a ReadList;
//   ReadList       raise this thread's entry.
void rd_set(ByteCell* c, uint32_t tid) {
    uint32_t now = vc_get(tid, tid);
    if (cell_rd_shared(c)) {
        readlist_raise(c->rd, tid, now);
        return;
    }
    if (thread_vc(tid).covers(c->rd)) {
        c->rd = epoch_make(tid, now);
        return;
    }
    // A concurrent read: the recorded epoch moves into a list.
    uint32_t off = readlist_new(c->rd);
    if (off == 0) {
        return;
    }
    readlist_raise(off, tid, now);
    c->rd = off;
    c->pub |= kRdSharedBit;
    atomic_fetch_add_u64(&g_header->rd_shared_count, 1);
}

void rd_reset(ByteCell* c) {
    if (cell_rd_shared(c)) {
        readlist_free(c->rd);
        c->pub &= ~kRdSharedBit;
    }
    c->rd = kEpochBot;
}

// ---- byte splitting -------------------------------------------------------

ByteCell* split_array(const ByteCell* wc) {
    return static_cast<ByteCell*>(arena_at(wc->rd));
}

void cell_reset_state(ByteCell* c) {
    rd_reset(c);
    uint32_t p = cell_pub_off(c);
    slotmap_release(&p);
    cell_set_pub_off(c, p);
    uint32_t a = cell_acq_off(c);
    slotmap_release(&a);
    cell_set_acq_off(c, a);
}

// Give every byte of a word its own cell, each inheriting the word's state.
// The inherited maps and read clock are copies, not shared offsets: eight
// cells pointing at one map would alias and double-free. Returns nullptr if
// the arena is exhausted; the caller then stays at word granularity.
ByteCell* ensure_split(ByteCell* wc) {
    if (cell_split(wc)) {
        return split_array(wc);
    }
    uint32_t off = arena_alloc(kClassSplit);
    if (off == 0) {
        return nullptr;
    }
    ByteCell* arr = static_cast<ByteCell*>(arena_at(off));
    bool ok = true;
    for (uint32_t i = 0; i < 8 && ok; ++i) {
        arr[i].wb = wc->wb;
        arr[i].rd = kEpochBot;
        arr[i].pub = wc->pub & kAtomicBit; // atomic_x is state too, and is inherited
        arr[i].acq = 0;
        if (!cell_rd_shared(wc)) {
            arr[i].rd = wc->rd;
        } else {
            uint32_t copy = readlist_copy(wc->rd);
            if (copy == 0) {
                ok = false;
                break;
            }
            arr[i].rd = copy;
            arr[i].pub |= kRdSharedBit;
        }
        for (uint32_t which = 0; which < 2; ++which) {
            uint32_t src_off = which == 0 ? cell_pub_off(wc) : cell_acq_off(wc);
            if (src_off == 0) {
                continue;
            }
            uint32_t dst_off = arena_alloc(kClassSlotMap);
            if (dst_off == 0) {
                ok = false;
                break;
            }
            SlotMap* src = slotmap_at(src_off);
            SlotMap* dst = slotmap_at(dst_off);
            // the whole block, so a traced map keeps its provenance
            std::memcpy(dst, src, arena_class_size(kClassSlotMap));
            dst->full_off = 0;
            if (src->full_off != 0) {
                uint32_t fo = arena_alloc(kClassFullMap);
                if (fo == 0) {
                    arena_free(kClassSlotMap, dst_off);
                    ok = false;
                    break;
                }
                std::memcpy(arena_at(fo), arena_at(src->full_off),
                            arena_class_size(kClassFullMap));
                dst->full_off = fo;
            }
            if (which == 0) {
                cell_set_pub_off(&arr[i], dst_off);
            } else {
                cell_set_acq_off(&arr[i], dst_off);
            }
        }
    }
    if (!ok) {
        // Unwind completely: a half-built split array would be worse than none.
        for (uint32_t i = 0; i < 8; ++i) {
            cell_reset_state(&arr[i]);
        }
        arena_free(kClassSplit, off);
        return nullptr;
    }
    if (g_prov) {
        uint32_t* pos = reinterpret_cast<uint32_t*>(arr + kWordBytes);
        for (uint32_t i = 0; i < kWordBytes; ++i) {
            pos[i] = g_cell_pos[wc - g_cells];
        }
    }
    cell_reset_state(wc);
    wc->wb = kEpochBot;
    wc->rd = off;
    wc->acq |= kSplitBit;
    atomic_fetch_add_u64(&g_header->split_count, 1);
    return arr;
}

void unsplit(ByteCell* wc) {
    if (!cell_split(wc)) {
        return;
    }
    uint32_t off = wc->rd;
    ByteCell* arr = static_cast<ByteCell*>(arena_at(off));
    for (uint32_t i = 0; i < 8; ++i) {
        cell_reset_state(&arr[i]);
    }
    arena_free(kClassSplit, off);
    wc->acq &= ~kSplitBit;
    wc->rd = kEpochBot;
    wc->wb = kEpochBot;
    wc->pub = 0;
    wc->acq = 0;
}


} // namespace csan
