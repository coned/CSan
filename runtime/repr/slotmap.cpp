// SPDX-License-Identifier: MIT
#include "repr/slotmap.h"


namespace csan {

// After promotion the inline slots are never read again.
bool slotmap_promote(SlotMap* m) {
    uint32_t off = arena_alloc(kClassFullMap);
    if (off == 0) {
        return false;
    }
    uint8_t* st = fullmap_states(off);
    uint32_t* clk = fullmap_clocks(off);
    uint32_t* clk_m = fullmap_clocks_m(off);
    SlotProv* prov = slotmap_prov(m);
    uint32_t* rec = fullmap_rec_pos(off);
    uint32_t* fin = fullmap_fin_pos(off);
    for (uint32_t i = 0; i < 4; ++i) {
        SlotState s = slot_state(m, i);
        if (s != kSlotNone) {
            uint32_t u = epoch_tid(m->inline_e[i]); // key == epoch_tid, see SlotMap
            if (u < g_threads) {
                st[u] = static_cast<uint8_t>(s);
                clk[u] = epoch_clock(m->inline_e[i]);
                clk_m[u] = m->inline_m[i];
                if (prov != nullptr) {
                    rec[u] = prov->rec[i];
                    fin[u] = prov->fin[i];
                }
            }
        }
    }
    m->full_off = off;
    atomic_fetch_add_u64(&g_header->promote_count, 1);
    return true;
}

bool slotmap_pend(SlotMap* m, uint32_t t, Epoch now, uint32_t pos) {
    if (m->full_off != 0) {
        uint8_t* st = fullmap_states(m->full_off);
        uint32_t* clk = fullmap_clocks(m->full_off);
        if (t >= g_threads || st[t] != kSlotNone) {
            return false;
        }
        st[t] = kSlotPending;
        clk[t] = epoch_clock(now);
        if (uint32_t* rec = fullmap_rec_pos(m->full_off)) {
            rec[t] = pos;
            fullmap_fin_pos(m->full_off)[t] = 0;
        }
        return true;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) != kSlotNone && epoch_tid(m->inline_e[i]) == t) {
            return false;
        }
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) == kSlotNone) {
            m->inline_e[i] = now;
            slot_set_state(m, i, kSlotPending);
            if (SlotProv* prov = slotmap_prov(m)) {
                prov->rec[i] = pos;
                prov->fin[i] = 0;
            }
            return true;
        }
    }
    if (!slotmap_promote(m)) {
        atomic_fetch_add_u64(&g_header->overflow_count, 1);
        return false;
    }
    return slotmap_pend(m, t, now, pos);
}

// The new state, E and E_r of an entry under `fn`, or false if `fn` leaves it:
//   ApplyFin:  Pending(E')  -> Finish(now)
//   ApplySFin: Pending(E')  -> AllowStore(now)
//   ApplyMFin: Pending(E')  -> AllowAccess(now, now)
//              AllowStore(E)   -> AllowAccess(E, now)
static bool apply_entry(SlotApply fn, SlotState& st, uint32_t& clk, uint32_t& clk_m, Epoch now) {
    uint32_t k = epoch_clock(now);
    switch (fn) {
    case kApplyFin:
    case kApplySFin:
        if (st != kSlotPending) {
            return false;
        }
        st = kSlotFinish; // Finish and AllowStore share a code
        clk = k;
        return true;
    case kApplyMFin:
        if (st == kSlotPending) {
            st = kSlotAllowAccess;
            clk = k;
            clk_m = k;
            return true;
        }
        if (st == kSlotAllowStore) {
            st = kSlotAllowAccess; // E_w stays
            clk_m = k;
            return true;
        }
        return false;
    }
    return false;
}

void slotmap_apply(SlotMap* m, uint32_t t, SlotApply fn, Epoch now, uint32_t pos) {
    if (m->full_off != 0) {
        if (t >= g_threads) {
            return;
        }
        uint8_t* st = fullmap_states(m->full_off);
        SlotState s = static_cast<SlotState>(st[t]);
        if (apply_entry(fn, s, fullmap_clocks(m->full_off)[t], fullmap_clocks_m(m->full_off)[t],
                        now)) {
            st[t] = s;
            if (uint32_t* fin = fullmap_fin_pos(m->full_off)) {
                fin[t] = pos;
            }
        }
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) == kSlotNone || epoch_tid(m->inline_e[i]) != t) {
            continue;
        }
        SlotState s = slot_state(m, i);
        uint32_t clk = epoch_clock(m->inline_e[i]);
        if (apply_entry(fn, s, clk, m->inline_m[i], now)) {
            slot_set_state(m, i, s);
            m->inline_e[i] = epoch_make(t, clk);
            if (SlotProv* prov = slotmap_prov(m)) {
                prov->fin[i] = pos; // one fence may finish many entries
            }
        }
        return;
    }
}

SlotState slotmap_state_of(uint32_t off, uint32_t t) {
    SlotMap* m = slotmap_at(off);
    if (m == nullptr) {
        return kSlotNone;
    }
    if (m->full_off != 0) {
        return t < g_threads ? static_cast<SlotState>(fullmap_states(m->full_off)[t]) : kSlotNone;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) != kSlotNone && epoch_tid(m->inline_e[i]) == t) {
            return slot_state(m, i);
        }
    }
    return kSlotNone;
}

// Every entry of `a` is in `b` with the same state and clocks (E_r only for
// AllowAccess).
static bool slotmap_within(uint32_t a, uint32_t b) {
    return !slotmap_for_each(a, [&](uint32_t u, SlotState st, Epoch E, Epoch E_r, uint32_t,
                                    uint32_t) {
        bool found = slotmap_for_each(b, [&](uint32_t v, SlotState st2, Epoch E2, Epoch E_r2,
                                             uint32_t, uint32_t) {
            return v == u && st2 == st && E2 == E && (st != kSlotAllowAccess || E_r2 == E_r);
        });
        return !found;
    });
}

bool slotmap_equal(uint32_t a, uint32_t b) {
    return slotmap_within(a, b) && slotmap_within(b, a);
}

bool slotmap_set(SlotMap* m, uint32_t t, SlotState st, uint32_t clk, uint32_t clk_m,
                 uint32_t pos) {
    if (m->full_off != 0) {
        if (t >= g_threads) {
            return false;
        }
        fullmap_states(m->full_off)[t] = static_cast<uint8_t>(st);
        fullmap_clocks(m->full_off)[t] = clk;
        fullmap_clocks_m(m->full_off)[t] = clk_m;
        if (uint32_t* rec = fullmap_rec_pos(m->full_off)) {
            rec[t] = pos;
            fullmap_fin_pos(m->full_off)[t] = pos;
        }
        return true;
    }
    uint32_t slot = 4;
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) != kSlotNone && epoch_tid(m->inline_e[i]) == t) {
            slot = i;
            break;
        }
        if (slot == 4 && slot_state(m, i) == kSlotNone) {
            slot = i;
        }
    }
    if (slot == 4) {
        if (!slotmap_promote(m)) {
            atomic_fetch_add_u64(&g_header->overflow_count, 1);
            return false;
        }
        return slotmap_set(m, t, st, clk, clk_m, pos);
    }
    m->inline_e[slot] = epoch_make(t, clk);
    m->inline_m[slot] = clk_m;
    slot_set_state(m, slot, st);
    if (SlotProv* prov = slotmap_prov(m)) {
        prov->rec[slot] = pos;
        prov->fin[slot] = pos;
    }
    return true;
}

void slotmap_clear_other_machines(uint32_t off, uint32_t mach) {
    SlotMap* m = slotmap_at(off);
    if (m == nullptr) {
        return;
    }
    if (m->full_off != 0) {
        uint8_t* st = fullmap_states(m->full_off);
        for (uint32_t u = 0, n = live_threads(); u < n; ++u) {
            if (st[u] != kSlotNone && machine_of(u) != mach) {
                st[u] = kSlotNone;
            }
        }
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) != kSlotNone && machine_of(epoch_tid(m->inline_e[i])) != mach) {
            slot_set_state(m, i, kSlotNone);
        }
    }
}

SlotMap* slotmap_get(uint32_t* poff) {
    if (*poff == 0) {
        uint32_t off = arena_alloc(kClassSlotMap);
        if (off == 0) {
            atomic_fetch_add_u64(&g_header->overflow_count, 1);
            return nullptr;
        }
        *poff = off;
    }
    return slotmap_at(*poff);
}

void slotmap_release(uint32_t* poff) {
    SlotMap* m = slotmap_at(*poff);
    if (m == nullptr) {
        return;
    }
    if (m->full_off != 0) {
        arena_free(kClassFullMap, m->full_off);
    }
    arena_free(kClassSlotMap, *poff);
    *poff = 0;
}

SlotMap* cell_pub_get(ByteCell* c) {
    uint32_t off = cell_pub_off(c);
    SlotMap* m = slotmap_get(&off);
    if (m != nullptr) {
        cell_set_pub_off(c, off);
    }
    return m;
}

SlotMap* cell_acq_get(ByteCell* c) {
    uint32_t off = cell_acq_off(c);
    SlotMap* m = slotmap_get(&off);
    if (m != nullptr) {
        cell_set_acq_off(c, off);
    }
    return m;
}

SlotMap* cell_pprev_get(ByteCell* c) {
    return slotmap_get(&c->pprev);
}

} // namespace csan
