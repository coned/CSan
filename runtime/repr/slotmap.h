// SPDX-License-Identifier: MIT
// SlotMap: the per-byte maps P_x : Tid -> PubSt and A_x : Tid -> AcqSt,
// inline or promoted (layout.h). These functions hide which.
#ifndef CSAN_SLOTMAP_H
#define CSAN_SLOTMAP_H

#include "repr/arena.h"
#include "state/layout.h"
#include "repr/vectorclock.h"

#include <cstdint>
#include <cstring>

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

// The entry's epoch E (E_w for AllowAccess), by tid.
inline uint32_t* fullmap_clocks(uint32_t off) {
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(arena_at(off)) +
                                       align_up(g_threads, 8));
}

// E_r of an AllowAccess entry, by tid.
inline uint32_t* fullmap_clocks_m(uint32_t off) {
    return fullmap_clocks(off) + g_threads;
}

// Provenance; null unless tracing.
inline SlotProv* slotmap_prov(SlotMap* m) {
    return g_prov ? reinterpret_cast<SlotProv*>(reinterpret_cast<uint8_t*>(m) +
                                                align_up(sizeof(SlotMap), 8))
                  : nullptr;
}

inline uint32_t* fullmap_rec_pos(uint32_t off) {
    if (!g_prov) {
        return nullptr;
    }
    uint64_t base = align_up(align_up(g_threads, 8) + static_cast<uint64_t>(g_threads) * 8, 8);
    return reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(arena_at(off)) + base);
}

inline uint32_t* fullmap_fin_pos(uint32_t off) {
    uint32_t* rec = fullmap_rec_pos(off);
    return rec != nullptr ? rec + g_threads : nullptr;
}

// Inline -> promoted. False if the arena is exhausted.
bool slotmap_promote(SlotMap* m);

// Start: t's entry becomes Pending(now) if it is None, so an earlier entry is
// kept. True if it changed;
// the caller queues it for t's next fence. `pos`: provenance when tracing.
bool slotmap_pend(SlotMap* m, uint32_t t, Epoch now, uint32_t pos);

// ApplyFin, ApplySFin, ApplyMFin on t's entry, with now = now(t) at
// the fence. `pos`: the fence's provenance.
enum SlotApply : uint8_t {
    kApplyFin,
    kApplySFin,
    kApplyMFin,
};
void slotmap_apply(SlotMap* m, uint32_t t, SlotApply fn, Epoch now, uint32_t pos);

// f(u, state, E, E_r, rec, fin) for each non-None entry of the map at `off`,
// until f returns true; returns whether it did. E_r means something only for
// AllowAccess. rec/fin are the entry's provenance when kProv (tracing), else 0.
// Inline, so a query costs what the loop costs.
template <bool kProv = false, class F>
inline bool slotmap_for_each(uint32_t off, F f) {
    SlotMap* m = slotmap_at(off);
    if (m == nullptr) {
        return false;
    }
    if (m->full_off == 0) {
        SlotProv* prov = kProv ? slotmap_prov(m) : nullptr;
        for (uint32_t i = 0; i < 4; ++i) {
            SlotState st = slot_state(m, i);
            if (st != kSlotNone &&
                f(epoch_tid(m->inline_e[i]), st, m->inline_e[i],
                  epoch_make(epoch_tid(m->inline_e[i]), m->inline_m[i]),
                  prov != nullptr ? prov->rec[i] : 0u, prov != nullptr ? prov->fin[i] : 0u)) {
                return true;
            }
        }
        return false;
    }
    uint8_t* st = fullmap_states(m->full_off);
    uint32_t* clk = fullmap_clocks(m->full_off);
    uint32_t* clk_m = fullmap_clocks_m(m->full_off);
    uint32_t* rec = kProv ? fullmap_rec_pos(m->full_off) : nullptr;
    uint32_t* fin = kProv ? fullmap_fin_pos(m->full_off) : nullptr;
    for (uint32_t u = 0, n = live_threads(); u < n; ++u) { // tids are never reused
        if (st[u] != kSlotNone &&
            f(u, static_cast<SlotState>(st[u]), epoch_make(u, clk[u]), epoch_make(u, clk_m[u]),
              rec != nullptr ? rec[u] : 0u, fin != nullptr ? fin[u] : 0u)) {
            return true;
        }
    }
    return false;
}

// t's entry of the map at `off`, kSlotNone if it has none.
SlotState slotmap_state_of(uint32_t off, uint32_t t);

// Whether the maps at `a` and `b` hold the same entries (offset 0 is empty).
bool slotmap_equal(uint32_t a, uint32_t b);

// t's entry becomes `st` with E = (t, clk) and E_r = (t, clk_m), whatever it
// was. False if the arena is exhausted.
bool slotmap_set(SlotMap* m, uint32_t t, SlotState st, uint32_t clk, uint32_t clk_m,
                 uint32_t pos);

// Every entry u with machine_of(u) != mach becomes ⊥.
void slotmap_clear_other_machines(uint32_t off, uint32_t mach);

// Allocates on first use. Null if the arena is exhausted. cell_pub_get /
// cell_acq_get do the same through a ByteCell's flag-bearing fields.
SlotMap* slotmap_get(uint32_t* poff);
void slotmap_release(uint32_t* poff);

// Every entry None (the store's P_x ↦ all-None); the blocks are kept for reuse.
inline void slotmap_clear(uint32_t off) {
    if (SlotMap* m = slotmap_at(off)) {
        m->inline_s = 0;
        if (m->full_off != 0) {
            std::memset(fullmap_states(m->full_off), 0, g_threads);
        }
    }
}
SlotMap* cell_pub_get(ByteCell* c);
SlotMap* cell_acq_get(ByteCell* c);
SlotMap* cell_pprev_get(ByteCell* c);

} // namespace csan

#endif
