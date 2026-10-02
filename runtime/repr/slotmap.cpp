// SPDX-License-Identifier: MIT
#include "repr/slotmap.h"

#include "base/trace.h"

namespace csan {

// After promotion the array is the only source of truth; the inline slots are
// never read again.
bool slotmap_promote(SlotMap* m) {
    uint32_t off = arena_alloc(kClassFullMap);
    if (off == 0) {
        return false;
    }
    uint8_t* st = fullmap_states(off);
    uint32_t* clk = fullmap_clocks(off);
    SlotProv* prov = slotmap_prov(m);
    uint32_t* rec = fullmap_rec_pos(off);
    uint32_t* fin = fullmap_fin_pos(off);
    for (uint32_t i = 0; i < 4; ++i) {
        SlotState s = slot_state(m, i);
        if (s != kSlotBot) {
            uint32_t u = epoch_tid(m->inline_e[i]); // key == epoch_tid, see SlotMap
            if (u < g_threads) {
                st[u] = static_cast<uint8_t>(s);
                clk[u] = epoch_clock(m->inline_e[i]);
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

// If t already has a slot in any state the map is unchanged: a later
// write-back or flush must not downgrade or refresh an existing witness.
bool slotmap_record(SlotMap* m, uint32_t t, Epoch e) {
    if (m->full_off != 0) {
        uint8_t* st = fullmap_states(m->full_off);
        uint32_t* clk = fullmap_clocks(m->full_off);
        if (t >= g_threads || st[t] != kSlotBot) {
            return false; // already present -> unchanged
        }
        st[t] = kSlotIntermediate;
        clk[t] = epoch_clock(e);
        if (uint32_t* rec = fullmap_rec_pos(m->full_off)) {
            rec[t] = trace_cur_pos();
            fullmap_fin_pos(m->full_off)[t] = 0;
        }
        return true;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) != kSlotBot && epoch_tid(m->inline_e[i]) == t) {
            return false;
        }
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) == kSlotBot) {
            m->inline_e[i] = e;
            slot_set_state(m, i, kSlotIntermediate);
            if (SlotProv* prov = slotmap_prov(m)) {
                prov->rec[i] = trace_cur_pos();
                prov->fin[i] = 0;
            }
            return true;
        }
    }
    if (!slotmap_promote(m)) {
        atomic_fetch_add_u64(&g_header->overflow_count, 1);
        return false;
    }
    return slotmap_record(m, t, e);
}

//   A[t ↦ acquired(E)]  if A[t] = flushed(E') and E' ⊑ E
// The slot is re-stamped with the fence's epoch: the value is only guaranteed
// visible once the fence completes.
void slotmap_finish(SlotMap* m, uint32_t t, Epoch fence_epoch) {
    if (m->full_off != 0) {
        if (t >= g_threads) {
            return;
        }
        uint8_t* st = fullmap_states(m->full_off);
        uint32_t* clk = fullmap_clocks(m->full_off);
        if (st[t] == kSlotIntermediate && epoch_le(epoch_make(t, clk[t]), t)) {
            st[t] = kSlotFinal;
            clk[t] = epoch_clock(fence_epoch);
            if (uint32_t* fin = fullmap_fin_pos(m->full_off)) {
                fin[t] = trace_cur_pos();
            }
        }
        return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        if (slot_state(m, i) == kSlotIntermediate && epoch_tid(m->inline_e[i]) == t &&
            epoch_le(m->inline_e[i], t)) {
            slot_set_state(m, i, kSlotFinal);
            m->inline_e[i] = fence_epoch;
            if (SlotProv* prov = slotmap_prov(m)) {
                prov->fin[i] = trace_cur_pos(); // one fence may finish many slots
            }
            return;
        }
    }
}

// The existential queries:
//   Pub^B(x,ℂ)   ⟺ ∃u,E. ℙ^B_x[u] = published(E) ∧ E ⊑ ℂ           (no machine)
//   Acq^B(x,m,ℂ) ⟺ ∃u,E. mach(u)=m ∧ 𝔸^B_x[u] = acquired(E) ∧ E ⊑ ℂ
bool slotmap_any_final(uint32_t off, uint32_t tid, bool same_machine) {
    SlotMap* m = slotmap_at(off);
    if (m == nullptr) {
        return false;
    }
    VectorClock c = thread_vc(tid);
    uint32_t my_machine = machine_of(tid);
    if (m->full_off == 0) {
        for (uint32_t i = 0; i < 4; ++i) {
            if (slot_state(m, i) != kSlotFinal) {
                continue;
            }
            Epoch e = m->inline_e[i];
            if (same_machine && machine_of(epoch_tid(e)) != my_machine) {
                continue;
            }
            if (c.covers(e)) {
                return true;
            }
        }
        return false;
    }
    uint8_t* st = fullmap_states(m->full_off);
    uint32_t* clk = fullmap_clocks(m->full_off);
    for (uint32_t u = 0; u < g_threads; ++u) {
        if (st[u] != kSlotFinal) {
            continue;
        }
        if (same_machine && machine_of(u) != my_machine) {
            continue;
        }
        if (clk[u] <= c.get(u)) { // epoch_make(u, clk[u]) ⊑ ℂ_t; key == tid
            return true;
        }
    }
    return false;
}

bool pub_ok(uint32_t off, uint32_t tid) {
    return slotmap_any_final(off, tid, /*same_machine=*/false);
}

bool acq_ok(uint32_t off, uint32_t tid) {
    return slotmap_any_final(off, tid, /*same_machine=*/true);
}

// Allocate the map on first use, so a byte that is never published or flushed
// costs one zero word instead of a map.
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

// Same, for a ByteCell's flag-bearing pub/acq fields.
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


} // namespace csan
