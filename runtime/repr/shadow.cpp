// SPDX-License-Identifier: MIT
#include "repr/shadow.h"

#include "base/atomics.h"
#include "repr/sync.h"

#include <cstring>

namespace csan {

// ---- ℝ_x storage ------------------------------------------------------------

void rd_to_list(ByteCell* c, uint32_t tid, uint32_t now) {
    uint32_t off = readlist_new(c->rd);
    if (off == 0) {
        return;
    }
    readlist_raise(off, tid, now);
    c->rd = off;
    c->pub |= kRdSharedBit;
    atomic_fetch_add_u64(&g_header->rd_shared_count, 1);
}

// ---- byte splitting -------------------------------------------------------

ByteCell* split_array(const ByteCell* wc) {
    return static_cast<ByteCell*>(arena_at(cell_rd_off(wc)));
}

void cell_reset_state(ByteCell* c) {
    rd_clear(c);
    uint32_t p = cell_pub_off(c);
    slotmap_release(&p);
    cell_set_pub_off(c, p);
    uint32_t a = cell_acq_off(c);
    slotmap_release(&a);
    cell_set_acq_off(c, a);
    uint32_t pp = c->pprev;
    slotmap_release(&pp);
    c->pprev = pp;
    c->wprev = kEpochBot;
}

// Maps and read lists are copied, not shared: shared offsets would double-free.
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
        arr[i].pub = wc->pub & (kAtomicBit | kSyncBit); // flags are inherited
        arr[i].acq = wc->acq & (kAtomicStoreBit | kReportedBit); // inherited, like the pub flags
        arr[i].wprev = wc->wprev;
        arr[i].pprev = 0;
        if (!cell_rd_shared(wc)) {
            arr[i].rd = wc->rd;
        } else {
            uint32_t copy = readlist_copy(cell_rd_off(wc));
            if (copy == 0) {
                ok = false;
                break;
            }
            arr[i].rd = copy;
            arr[i].pub |= kRdSharedBit;
        }
        for (uint32_t which = 0; which < 3; ++which) {
            uint32_t src_off = which == 0   ? cell_pub_off(wc)
                               : which == 1 ? cell_acq_off(wc)
                                            : wc->pprev.load();
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
            // whole block, to keep provenance
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
            } else if (which == 1) {
                cell_set_acq_off(&arr[i], dst_off);
            } else {
                arr[i].pprev = dst_off;
            }
        }
    }
    if (!ok) {
        // Unwind fully: a half-built split is worse than none.
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
    wc->pub &= ~kSyncBit;
    wc->acq |= kSplitBit;
    atomic_fetch_add_u64(&g_header->split_count, 1);
    return arr;
}

void unsplit(ByteCell* wc) {
    if (!cell_split(wc)) {
        return;
    }
    uint32_t off = cell_rd_off(wc);
    ByteCell* arr = static_cast<ByteCell*>(arena_at(off));
    uint32_t sync = 0;
    for (uint32_t i = 0; i < 8; ++i) {
        sync |= arr[i].pub & kSyncBit;
        cell_reset_state(&arr[i]);
    }
    arena_free(kClassSplit, off);
    wc->rd = kEpochBot;
    wc->wb = kEpochBot;
    wc->pub = sync;
    wc->acq = 0;
    wc->wprev = kEpochBot;
    wc->pprev = 0;
}

// The state a store leaves that two bytes must share to be one cell again.
static bool same_state(const ByteCell* a, const ByteCell* b) {
    constexpr uint32_t kPubFlags = kRdSharedBit | kAtomicBit | kSyncBit;
    return a->wb == b->wb && a->rd == b->rd && a->wprev == b->wprev &&
           (a->pub & kPubFlags) == (b->pub & kPubFlags) &&
           (a->acq & (kAtomicStoreBit | kReportedBit)) == (b->acq & (kAtomicStoreBit | kReportedBit)) &&
           !cell_rd_shared(a) &&
           slotmap_equal(cell_pub_off(a), cell_pub_off(b)) &&
           slotmap_equal(cell_acq_off(a), cell_acq_off(b)) && slotmap_equal(a->pprev, b->pprev);
}

bool collapse_if_uniform(ByteCell* wc) {
    if (!cell_split(wc)) {
        return false;
    }
    ByteCell* arr = split_array(wc);
    for (uint32_t i = 1; i < kWordBytes; ++i) {
        if (!same_state(&arr[0], &arr[i])) {
            return false;
        }
    }
    ByteCell keep = arr[0];
    cell_set_pub_off(&arr[0], 0); // its maps move to the word cell
    cell_set_acq_off(&arr[0], 0);
    arr[0].pprev = 0;
    unsplit(wc);
    wc->wb = keep.wb;
    wc->rd = keep.rd;
    wc->pub |= keep.pub & kAtomicBit;
    wc->acq |= keep.acq & (kAtomicStoreBit | kReportedBit);
    cell_set_pub_off(wc, cell_pub_off(&keep));
    cell_set_acq_off(wc, cell_acq_off(&keep));
    wc->wprev = keep.wprev;
    wc->pprev = keep.pprev;
    return true;
}

void mark_release_site(uint64_t key) {
    ByteCell* wc = &g_cells[key / kWordBytes];
    // Lock-free fast path: a flagged word cell is unsplit (split moves the flag
    // to its bytes), and a write that cleared it ended S_x first.
    if ((wc->pub & kSyncBit) != 0) {
        return;
    }
    LineCell* lc = &g_lines[key / kLineBytes];
    line_lock(lc);
    ByteCell* c = cell_split(wc) ? &split_array(wc)[key % kWordBytes] : wc;
    c->pub |= kSyncBit;
    line_unlock(lc);
}

void end_flagged_sequences(ByteCell* wc, uint64_t lo, uint64_t hi) {
    if (!cell_split(wc)) {
        if (hi - lo == kWordBytes) {
            wc->pub &= ~kSyncBit;
        }
        for (uint64_t k = lo; k < hi; ++k) {
            SyncRef(k).erase();
        }
        return;
    }
    ByteCell* bytes = split_array(wc);
    for (uint64_t k = lo; k < hi; ++k) {
        ByteCell* c = &bytes[k % kWordBytes];
        if ((c->pub & kSyncBit) != 0) {
            c->pub &= ~kSyncBit;
            SyncRef(k).erase();
        }
    }
}

} // namespace csan
