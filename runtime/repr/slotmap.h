// SPDX-License-Identifier: MIT
// Operations on SlotMap, the per-byte maps thread -> PSlot (publications)
// and thread -> ASlot (acquisitions).
//
// A map is in one of two modes and never a mixture; SlotMap in layout.h gives
// the field layout of each. These functions hide which mode a map is in.
#ifndef CSAN_SLOTMAP_H
#define CSAN_SLOTMAP_H

#include "repr/arena.h"
#include "state/layout.h"
#include "repr/vectorclock.h"

#include <cstdint>

namespace csan {

inline SlotMap* slotmap_at(uint32_t off) {
    return off != 0 ? static_cast<SlotMap*>(arena_at(off)) : nullptr;
}

inline SlotState slot_state(const SlotMap* m, uint32_t i) {
    return static_cast<SlotState>((m->inline_s >> (2 * i)) & 3);
}

inline void slot_set_state(SlotMap* m, uint32_t i, SlotState s) {
    m->inline_s = static_cast<uint8_t>((m->inline_s & ~(3u << (2 * i))) |
                                       (static_cast<uint32_t>(s) << (2 * i)));
}

inline uint8_t* fullmap_states(uint32_t off) {
    return static_cast<uint8_t*>(arena_at(off));
}

inline uint32_t* fullmap_clocks(uint32_t off) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(arena_at(off)) +
                                       align_up(g_threads, 8));
}

// Provenance, under tracing only (null otherwise): an inline map's SlotProv,
// and a promoted map's record and finish Pos, each indexed by tid.
inline SlotProv* slotmap_prov(SlotMap* m) {
    return g_prov ? reinterpret_cast<SlotProv*>(reinterpret_cast<uint8_t*>(m) +
                                                align_up(sizeof(SlotMap), 8))
                  : nullptr;
}

inline uint32_t* fullmap_rec_pos(uint32_t off) {
    if (!g_prov) {
        return nullptr;
    }
    uint64_t base = align_up(align_up(g_threads, 8) + static_cast<uint64_t>(g_threads) * 4, 8);
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(arena_at(off)) + base);
}

inline uint32_t* fullmap_fin_pos(uint32_t off) {
    uint32_t* rec = fullmap_rec_pos(off);
    return rec != nullptr ? rec + g_threads : nullptr;
}


// Moves an inline map to the promoted, tid-indexed form, carrying the four
// inline entries with it. False if the arena is exhausted.
bool slotmap_promote(SlotMap* m);

// RecordWB / RecordFlush: record `e` for thread `t` unless `t` already has an
// entry, in which case the map is unchanged -- the EARLIEST witness wins.
// True when a new record was stored, so the caller queues it for the fence.
bool slotmap_record(SlotMap* m, uint32_t t, Epoch e);

// FinishPub / FinishAcq for key `t` alone: if that slot is intermediate with
// an epoch E' ⊑ ℂ_t, make it final and re-stamp it with the fence's epoch.
void slotmap_finish(SlotMap* m, uint32_t t, Epoch fence_epoch);

// Is there ANY thread whose slot is final with an epoch `tid` already knows?
// `same_machine` additionally restricts to threads on tid's machine, which is
// what separates the Acq queries from the Pub ones.
bool slotmap_any_final(uint32_t off, uint32_t tid, bool same_machine);
bool pub_ok(uint32_t off, uint32_t tid);
bool acq_ok(uint32_t off, uint32_t tid);

// Fetch the map at *poff, allocating one on first use. Null if the arena is
// exhausted. cell_pub_get / cell_acq_get do the same through a ByteCell's
// flag-bearing fields.
SlotMap* slotmap_get(uint32_t* poff);
void slotmap_release(uint32_t* poff);
SlotMap* cell_pub_get(ByteCell* c);
SlotMap* cell_acq_get(ByteCell* c);

} // namespace csan

#endif
