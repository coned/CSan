// SPDX-License-Identifier: MIT
// The checker's rules, one function each, named as in the paper's detection
// section (sec:detect): rule_load is Load, rule_store Store, rule_writeback
// Writeback, rule_flush Flush, rule_sfence SFence, rule_mfence MFence; the
// queries PubB, PubPrev, AcqR, AcqW, the predicates CanPub, CanPubPrev,
// CanAcq and the steps Start, ApplyFin, ApplySFin, ApplyMFin keep their names
// too. Callers pass a
// valid tid and, where a shadow cell is needed, a CXL address.
#ifndef CSAN_RULES_H
#define CSAN_RULES_H

#include "state/layout.h"
#include "state/state.h"
#include "repr/slotmap.h"
#include "repr/vectorclock.h"

#include <cstddef>
#include <cstdint>

namespace csan {

// ---- queries (rules_visibility.cpp) ----
//   PubB(x,C)   ⟺ ∃u,E.     P_x[u] = Finish(E) ∧ E ⪯ C
//   PubPrev(x,C) ⟺ ∃u,E.    P'_x[u] = Finish(E) ∧ E ⪯ C
//   AcqR(x,m,C) ⟺ ∃u,E_w,E_r. mach(u) = m ∧ A_x[u] = AllowAccess(E_w,E_r) ∧ E_r ⪯ C
//   AcqW(x,m,C) ⟺ ∃u,E_w,E_r. mach(u) = m ∧ A_x[u] ∈ {AllowStore(E_w), AllowAccess(E_w,E_r)} ∧ E_w ⪯ C
// Here C = C_t and m = mach(t).
enum Query : uint8_t {
    kQueryPubB,
    kQueryPubPrev,
    kQueryAcqR,
    kQueryAcqW,
};

// Whether entry u of P_x, P'_x or A_x, in state st with epochs E and E_r, is a
// witness of `q` for t. Shared by the queries and the trace's account of them.
inline bool query_entry(Query q, uint32_t u, SlotState st, Epoch E, Epoch E_r,
                        const VectorClock& c, uint32_t t) {
    switch (q) {
    case kQueryPubB:
    case kQueryPubPrev:
        return st == kSlotFinish && c.covers(E);
    case kQueryAcqR:
        return st == kSlotAllowAccess && machine_of(u) == machine_of(t) && c.covers(E_r);
    case kQueryAcqW:
        return (st == kSlotAllowStore || st == kSlotAllowAccess) && machine_of(u) == machine_of(t) &&
               c.covers(E);
    }
    return false;
}

bool pub_b(const ByteCell* x, uint32_t t); // PubB(x, C_t)
bool acq_r(const ByteCell* x, uint32_t t); // AcqR(x, mach(t), C_t)
bool acq_w(const ByteCell* x, uint32_t t); // AcqW(x, mach(t), C_t)
bool pub_prev(const ByteCell* x, uint32_t t); // PubPrev(x, C_t)

// ---- the premises Load and Store add (rules_access.cpp) ----

// Load:          W_x = ⊥ ∨ mach(W_x) = mach(t) ∨ AcqR(x, mach(t), C_t)
bool load_acq_premise(const ByteCell* x, uint32_t t);

// Store, x ∈ X:  W_x = ⊥ ∨ mach(W_x) = mach(t) ∨ PubB(x, C_t)
// Beyond the paper: the last store to x was atomic, whose write-back the
// runtime issues itself (push_atomic_write).
bool store_pub_premise(const ByteCell* x, uint32_t t);

// Store, y ∈ c ∖ X:  W_y = ⊥ ∨ AcqW(y, mach(t), C_t)
// Also when mach(W_y) = mach(t): A_y then holds how t's machine made its copy
// of y no older than W'_y (cases 2a and 2b of the paper).
bool store_acq_premise(const ByteCell* y, uint32_t t);

// The Store update of one byte, with now = now(t):
//   W_x ↦ now, R_x ↦ ⊥, P_x ↦ all-None, (W'_x, P'_x) ↦ (w, q), A_x[t] ↦ a,
//   ∀u. mach(u) ≠ mach(t): A_x[u] ↦ ⊥
//   (w, q) = (W_x, P_x)   if W_x ≠ ⊥ ∧ mach(W_x) ≠ mach(t), else (W'_x, P'_x)
//   a = AllowAccess(⊥, ⊥)       if W_x = ⊥ (derived, not stored: acq_r/acq_w)
//       AllowAccess(now, now)   if W_x ≠ ⊥ ∧ mach(W_x) ≠ mach(t) ∧ A_x[t] ∈ {None, Pending(E)}
//       A_x[t]                  otherwise
void store_update(ByteCell* x, uint32_t t, Epoch now);

// ---- Load and Store ----
// A store is one event on the bytes X it writes in each cache line c; its
// premise on the other bytes ranges over c ∖ X.

// Load and Store for every byte of [begin, begin+n).
void rule_load(uintptr_t begin, size_t n, uint32_t t);
void rule_store(uintptr_t begin, size_t n, uint32_t t);

// `atomic`: the shadow half of the atomic rules, which the paper leaves out.
// Their happens-before premises are discharged if the last access was atomic
// too, and an atomic load skips the AcqR premise. load_range / store_range
// leave S_x / V_x to the atomic rules.
void load_range(uintptr_t begin, size_t n, uint32_t t, bool atomic);
void store_range(uintptr_t begin, size_t n, uint32_t t, bool atomic);

// ---- the fences' effect on the entries ----
// SFence: ∀x. P_x[t], P'_x[t] ↦ ApplyFin(·, now(t)),  A_x[t] ↦ ApplySFin(A_x[t], now(t))
// MFence: ∀x. P_x[t], P'_x[t] ↦ ApplyFin(·, now(t)),  A_x[t] ↦ ApplyMFin(A_x[t], now(t))
// With now(t) = `now`; also used by the atomic rules for a locked
// instruction, which acts as an mfence.
void apply_sfence(uint32_t t, Epoch now);
void apply_mfence(uint32_t t, Epoch now);

// ---- Writeback, Flush, SFence, MFence ----

void rule_writeback(uintptr_t begin, size_t n, uint32_t t); // Writeback
// Flush. Returns how many lines it started an acquisition on, for the trace.
uint32_t rule_flush(uintptr_t begin, size_t n, uint32_t t);
void rule_sfence(uint32_t t); // SFence
void rule_mfence(uint32_t t); // MFence

// ---- atomics ----
// The memory operation runs in the same critical section as its clock effect,
// so no thread sees a value without the clock its release published.
// `shadow`: also run the shadow half (false outside the pool).
// `mfenced`: lowers to a full barrier (lock prefix / xchg), so also applies
// the MFence effect (apply_mfence). Set by the pass, not the memory order.
uint64_t rule_load_rlx(const void* addr, uint32_t size, uint32_t t, bool shadow);
uint64_t rule_load_acq(const void* addr, uint32_t size, uint32_t t, bool shadow);
void rule_store_rlx(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow);
void rule_store_rel(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow,
                    bool mfenced);

// RmwRlx, RmwAcq, RmwRel, RmwAcqRel.
uint64_t rule_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t t,
                  bool acquire, bool release, bool shadow, bool mfenced);

// CasOk (Rmw rule) / CasFail (load rule). `mfenced` applies to both: the
// prefix fences whether or not the comparison matched.
uint64_t rule_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size, uint32_t t,
                  bool ok_acquire, bool ok_release, bool fail_acquire, bool shadow,
                  bool mfenced);

// Clock halves, for manual hooks whose caller did the access. Each takes its
// location's stripe.
void load_half(uintptr_t x, uint32_t t, bool acquire);                // C_t or F^acq_t ⊔= S_x
bool store_half(uintptr_t x, size_t size, uint32_t t, bool release); // S_x <- C_t or F^rel_t
bool rmw_half(uintptr_t x, uint32_t t, bool release);                // S_x ⊔= C_t or F^rel_t

// ---- IR (C++) fences ----

void rule_fence_rel(uint32_t t);    // FenceRel
void rule_fence_acq(uint32_t t);    // FenceAcq
void rule_fence_acqrel(uint32_t t); // FenceAcqRel

// ---- locks and threads ----

void rule_lock(uintptr_t a, uint32_t t);   // Lock
void rule_unlock(uintptr_t a, uint32_t t); // Unlock

// Caller holds the global lock (thread bookkeeping for u). `t` may be
// UINT32_MAX: no parent id, u starts fresh.
void rule_fork_locked(uint32_t t, uint32_t u);
void rule_join_locked(uint32_t t, uint32_t u);

// Fork of a child process: ℂ_u ← parent_at_fork[u ↦ 1]. The parent ticked
// itself at fork (thread.cpp).
void rule_fork_process_locked(const VectorClock& parent_at_fork, uint32_t u);

} // namespace csan

#endif
