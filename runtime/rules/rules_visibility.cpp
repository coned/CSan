// SPDX-License-Identifier: MIT
// The rules Writeback, Flush, SFence and MFence, the predicates CanPub,
// CanPubPrev and CanAcq, the steps Start, ApplyFin, ApplySFin and ApplyMFin,
// and the queries PubB, PubPrev, AcqR and AcqW.
// The fence rules apply their functions to every byte; an entry of t they
// change is one Start made Pending, or ApplySFin made
// AllowStore, so each such entry is queued in pending_queue(t) and a fence
// visits exactly t's queues. Exact, since only t's fences change t's entries.
#include "rules/rules.h"

#include "base/atomics.h"
#include "base/report.h"
#include "base/trace.h"
#include "repr/shadow.h"
#include "repr/slotmap.h"
#include "repr/walk.h"

#include <algorithm>

namespace csan {

// ---- queries (formulas in rules.h) ----

// ∃u: query_entry(q, u, ...) over the map at `off`.
static bool query_any(Query q, uint32_t off, uint32_t t) {
    VectorClock c = thread_vc(t);
    return slotmap_for_each(off, [&](uint32_t u, SlotState st, Epoch E, Epoch E_r, uint32_t,
                                     uint32_t) { return query_entry(q, u, st, E, E_r, c, t); });
}

bool pub_b(const ByteCell* x, uint32_t t) {
    return query_any(kQueryPubB, cell_pub_off(x), t);
}

// Whether A_x holds the AllowAccess(⊥, ⊥) entry that Store sets over the
// initial value, for a thread of mach(t). Not stored: the entry is set by the
// first store to x, which no store of another machine has replaced yet, and
// only such a store resets it, setting W'_x ≠ ⊥ for good. So it is present
// exactly when W_x ≠ ⊥ ∧ W'_x = ⊥ ∧ mach(W_x) = mach(t), and since ⊥ ⪯ C_t
// it is a witness of both AcqR and AcqW.
static bool init_allow_access(const ByteCell* x, uint32_t t) {
    return x->wb != kEpochBot && x->wprev == kEpochBot &&
           machine_of(epoch_tid(x->wb)) == machine_of(t);
}

bool acq_r(const ByteCell* x, uint32_t t) {
    return init_allow_access(x, t) || query_any(kQueryAcqR, cell_acq_off(x), t);
}

bool acq_w(const ByteCell* x, uint32_t t) {
    return init_allow_access(x, t) || query_any(kQueryAcqW, cell_acq_off(x), t);
}

bool pub_prev(const ByteCell* x, uint32_t t) {
    return query_any(kQueryPubPrev, x->pprev, t);
}

// ---- CanPub, CanPubPrev, CanAcq and Start ----

// Start: Pending(now(t)) into t's entry if it is None. `m` is null if the arena
// is exhausted. True if the entry changed; the caller queues it for t's next
// fence.
static bool pend(SlotMap* m, uint32_t t, Epoch now) {
    return m != nullptr && slotmap_pend(m, t, now, trace_pos());
}

// CanPub(x,m,C) ⟺ W ≠ ⊥ ∧ W ⪯ C ∧ mach(W) = m, for W = W_x; CanPubPrev is
// the same for W = W'_x.
static bool can_pub(Epoch w, uint32_t t) {
    return w != kEpochBot && epoch_le(w, t) && machine_of(epoch_tid(w)) == machine_of(t);
}

// The publication half of Writeback and Flush:
//   P_x[t]  ↦ Start(P_x[t], now(t))    if CanPub(x, mach(t), C_t)
//   P'_x[t] ↦ Start(P'_x[t], now(t))   if CanPubPrev(x, mach(t), C_t)
// Both maps of x sit behind one queue entry; a fence finishes both.
static void pub_pend(ByteCell* x, uintptr_t addr, uint32_t t, Epoch now) {
    bool ok = can_pub(x->wb, t);
    bool ok_prev = can_pub(x->wprev, t);
    bool queued = ok && pend(cell_pub_get(x), t, now);
    queued = (ok_prev && pend(cell_pprev_get(x), t, now)) || queued;
    if (queued) {
        pending_queue(t, kPendingPub).push_back(Pending{addr, now});
    }
    trace_access("WB", addr, t, ok || ok_prev ? "publication pending" : "nothing to publish");
}

// CanAcq(x,m,C) ⟺ W_x ≠ ⊥ ∧ ((mach(W_x) ≠ m ∧ PubB(x,C)) ∨ (mach(W_x) = m ∧ PubPrev(x,C)))
static bool can_acq(const ByteCell* x, uint32_t t) {
    if (x->wb == kEpochBot) {
        return false;
    }
    return machine_of(epoch_tid(x->wb)) != machine_of(t) ? pub_b(x, t) : pub_prev(x, t);
}

// The acquisition half of Flush:
//   A_x[t] ↦ Start(A_x[t], now(t))   if CanAcq(x, mach(t), C_t)
// Returns whether CanAcq held. Start changes only None, so with A_x[t] set the
// query is skipped and t's standing acquisition is counted.
static bool acq_pend(ByteCell* x, uintptr_t addr, uint32_t t, Epoch now) {
    if (slotmap_state_of(cell_acq_off(x), t) != kSlotNone) {
        trace_access("FLUSH", addr, t, "acquisition already held");
        return true;
    }
    bool ok = can_acq(x, t);
    if (ok && pend(cell_acq_get(x), t, now)) {
        pending_queue(t, kPendingAcq).push_back(Pending{addr, now});
    }
    trace_access("FLUSH", addr, t,
                 ok ? "acquisition pending"
                    : "no acquisition: no finished publication happens before this flush");
    return ok;
}

// ---- ApplyFin, ApplySFin, ApplyMFin ----

// `fn` on t's entry of each byte in t's queue of `kind`. A word split since
// it was queued holds the entry in each of its byte cells; one collapsed
// since was reset by a store. `then`, if set, keeps the entries.
static void apply_queue(uint32_t t, PendingKind kind, SlotApply fn, Epoch now,
                        std::vector<Pending>* then = nullptr) {
    std::vector<Pending>& queue = pending_queue(t, kind);
    bool pub = kind == kPendingPub;
    uint32_t pos = trace_pos();
    LineCell* held = nullptr; // one lock per run of entries on the same line
    for (const Pending& p : queue) {
        LineCell* lc = line_cell_at(p.addr);
        if (lc != held) {
            if (held != nullptr) {
                line_unlock(held);
            }
            line_lock(lc);
            held = lc;
        }
        for_each_cell_of_word(p.addr, [&](ByteCell* x) {
            if (SlotMap* m = slotmap_at(pub ? cell_pub_off(x) : cell_acq_off(x))) {
                slotmap_apply(m, t, fn, now, pos);
            }
            if (SlotMap* m = pub ? slotmap_at(x->pprev) : nullptr) {
                slotmap_apply(m, t, fn, now, pos); // P'_x, where a store may have moved P_x
            }
        });
    }
    if (held != nullptr) {
        line_unlock(held);
    }
    if (then != nullptr) {
        then->insert(then->end(), queue.begin(), queue.end());
    }
    queue.clear();
}

// SFence's effect. The entries ApplySFin raised to AllowStore wait for t's
// next MFence.
void apply_sfence(uint32_t t, Epoch now) {
    apply_queue(t, kPendingPub, kApplyFin, now);
    apply_queue(t, kPendingAcq, kApplySFin, now, &pending_queue(t, kPendingAcqAllowStore));
}

// MFence's effect.
void apply_mfence(uint32_t t, Epoch now) {
    apply_queue(t, kPendingPub, kApplyFin, now);
    apply_queue(t, kPendingAcq, kApplyMFin, now);
    apply_queue(t, kPendingAcqAllowStore, kApplyMFin, now);
}

// ---- the rules ----
// Each reads the pre-state, takes now(t), then advances t's clock.

// Each line of [begin, begin+n) under its lock. Hardware-coherent lines are
// skipped (the caller still ticks).
template <class F>
static void for_each_line_locked(uintptr_t begin, size_t n, F f) {
    Shadow s = shadow_of(begin);
    uintptr_t end = begin + static_cast<uintptr_t>(n);
    for (uintptr_t line = begin & ~(kLineBytes - 1); line < end; line += kLineBytes) {
        LineCell* lc = s.line(line);
        if (line_is_hwcc(lc)) {
            continue;
        }
        line_lock(lc);
        f(s, line);
        line_unlock(lc);
    }
}

// Writeback:  ∀x ∈ c. P_x[t], P'_x[t] as in pub_pend
void rule_writeback(uintptr_t begin, size_t n, uint32_t t) {
    Epoch now = now_epoch(t);
    for_each_line_locked(begin, n, [&](const Shadow& s, uintptr_t line) {
        for_each_byte_cell(s, line, [&](ByteCell* x, uintptr_t addr) {
            if (x->wb != kEpochBot) { // never written: W'_x = ⊥ too, nothing to publish
                pub_pend(x, addr, t, now);
            }
        });
    });
    tick(t);
}

// Flush:  ∀x ∈ c. P_x[t], P'_x[t] as in Writeback, A_x[t] as in acq_pend
// acq_pend reads the pre-state: pub_pend only makes entries Pending, which
// PubB and PubPrev do not count, so a flush never acquires its own publication.
uint32_t rule_flush(uintptr_t begin, size_t n, uint32_t t) {
    Epoch now = now_epoch(t);
    uint32_t lines = 0;
    for_each_line_locked(begin, n, [&](const Shadow& s, uintptr_t line) {
        bool any = false;
        for_each_byte_cell(s, line, [&](ByteCell* x, uintptr_t addr) {
            if (x->wb == kEpochBot) { // never written: CanPub, CanPubPrev, CanAcq all fail
                return;
            }
            pub_pend(x, addr, t, now);
            any = acq_pend(x, addr, t, now) || any;
        });
        lines += any ? 1 : 0;
    });
    tick(t);
    return lines;
}

// SFence:  ∀x. P_x[t], P'_x[t] ↦ ApplyFin(·, now(t)),  A_x[t] ↦ ApplySFin(A_x[t], now(t))
void rule_sfence(uint32_t t) {
    apply_sfence(t, now_epoch(t));
    tick(t);
}

// MFence:  ∀x. P_x[t], P'_x[t] ↦ ApplyFin(·, now(t)),  A_x[t] ↦ ApplyMFin(A_x[t], now(t))
void rule_mfence(uint32_t t) {
    apply_mfence(t, now_epoch(t));
    tick(t);
}

} // namespace csan
