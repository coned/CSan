// SPDX-License-Identifier: MIT
// The checker's rules, one function each: rule_read is Read, rule_store_rel
// is StoreRel, wb_state is the write-back state transformation WB_{t,l,E}.
// The entry points in api_*.cpp only validate arguments and call in here.
//
//   rules/rules_access.cpp      Read, Write, Install, ReadVis, WriteVis, and the walk
//                         over a byte range that applies them per cell
//   rules/rules_visibility.cpp  WriteBack, Flush, SFence, MFence and the WB/FL/SF/MF
//                         state transformations they apply
//   rules/rules_sync.cpp        LoadRlx/LoadAcq, StoreRel/StoreRlx, the Rmw rules,
//                         CasOk/CasFail, the IR fences, Lock/Unlock, Fork/Join
//
// Two properties, tracked side by side:
//   VISIBILITY      was the line written back and fenced before another machine
//                   read it? The P and A relays.
//   HAPPENS-BEFORE  are the two accesses ordered at all? The thread clocks.
//
// Every function assumes a valid thread id and, where it needs a shadow cell,
// an address inside CXL memory.
#ifndef CSAN_RULES_H
#define CSAN_RULES_H

#include "state/layout.h"
#include "state/state.h"
#include "repr/vectorclock.h"

#include <cstddef>
#include <cstdint>

namespace csan {

// ---- visibility predicates ---------------------------------------------------

// ReadVis(x,t)  ⟺  mach(thr(W^B_x)) = mach(t)  ∨  Acq^B(x, mach(t), ℂ_t)
bool read_vis(const ByteCell* x, uint32_t t);

// WriteVis(ℓ,t) ⟺  mach(thr(W^L_ℓ)) = mach(t)  ∨  Acq^L(ℓ, mach(t), ℂ_t)
bool write_vis(const LineCell* l, uint32_t t);

// WriteVisFull(ℓ,t) ⟺  mach(thr(W^L_ℓ)) = mach(t)  ∨  Pub^L(ℓ, ℂ_t)  ∨  Acq^L(ℓ, mach(t), ℂ_t)
// The same premise for a store covering the whole line: no invalidate needed,
// the previous writer's publication still is. Strictly weaker than write_vis.
bool write_vis_full_line(const LineCell* l, uint32_t t);

// Install_{x,E}: W^B_x, W^L_ℓ ← E;  ℝ_x ← 0;  ℙ^B_x, 𝔸^B_x, ℙ^L_ℓ, 𝔸^L_ℓ ← ∅
void install(ByteCell* x, LineCell* l, Epoch E);

// ---- plain accesses ----------------------------------------------------------

// Read(x,t) and Write(x,t) for every byte of [begin, begin+n).
void rule_read(uintptr_t begin, size_t n, uint32_t t);
void rule_write(uintptr_t begin, size_t n, uint32_t t);

// The same walks with `atomic` set: the shadow half of the atomic rules, whose
// ordering premises are discharged when the byte's previous access was atomic
// too, and which record atomic_x ← ⊤. The visibility premises still apply.
// write_range does not touch 𝕊_x / 𝕍_x; each store rule does that itself.
void read_range(uintptr_t begin, size_t n, uint32_t t, bool atomic);
void write_range(uintptr_t begin, size_t n, uint32_t t, bool atomic);

// ---- state transformations of write-back, flush and fences --------------------
//
// wb_state and fl_state act on one line; the caller holds that line's lock.
// sf_state and mf_state act on every record this thread left.

void wb_state(LineCell* lc, uintptr_t line, uint32_t t, Epoch E); // WB_{t,ℓ,E}
// What one line's flush recorded: a line acquisition, and a byte acquisition
// on at least one of its bytes. Each needs the producer's publication already
// in this thread's clock.
struct FlushResult {
    bool line;
    bool byte;
};
FlushResult fl_state(LineCell* lc, uintptr_t line, uint32_t t, Epoch E); // FL_{t,ℓ,E}
void sf_state(uint32_t t, Epoch E);                               // SF_{t,E}
void mf_state(uint32_t t, Epoch E);                               // MF_{t,E}

// ---- write-back, flush and the x86 fences ------------------------------------

void rule_writeback(uintptr_t begin, size_t n, uint32_t t); // WriteBack
// Flush. Returns flush_verdict() of the lines that recorded each kind of
// acquisition, which the trace keeps with the flush event.
uint32_t rule_flush(uintptr_t begin, size_t n, uint32_t t);
void rule_sfence(uint32_t t);                               // SFence
void rule_mfence(uint32_t t);                               // MFence

// ---- atomic loads, stores, read-modify-writes ----------------------------------
//
// Each performs the real memory operation inside the same critical section as
// its clock effect, so no thread can observe a value without also observing
// the clock its release published. `shadow` says whether to also run the
// shadow half (the premises and Install); it is false for addresses outside
// the pool, which have no shadow cell.

// `mfenced` says the operation lowers to a full machine barrier -- `lock`-
// prefixed or `xchg` -- and so applies MF_{t,E} to the relays as well as its own
// rule. It comes from the pass, not from the memory order.
uint64_t rule_load_rlx(const void* addr, uint32_t size, uint32_t t, bool shadow);
uint64_t rule_load_acq(const void* addr, uint32_t size, uint32_t t, bool shadow);
void rule_store_rlx(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow);
void rule_store_rel(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow,
                    bool mfenced);

// RmwRlx, RmwAcq, RmwRel, RmwAcqRel: the two booleans say whether the read
// half acquires and whether the write half releases.
uint64_t rule_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t t,
                  bool acquire, bool release, bool shadow, bool mfenced);

// CasOk when the comparison succeeds (the Rmw rule at the success ordering),
// CasFail when it does not (the load rule at the failure ordering). `mfenced`
// applies to both: the prefix fences whether or not the comparison matched.
uint64_t rule_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size, uint32_t t,
                  bool ok_acquire, bool ok_release, bool fail_acquire, bool shadow,
                  bool mfenced);

// The halves the rules above are composed from, for callers that hold the
// global lock and have already performed the memory operation themselves.
void load_half_acq_locked(uintptr_t x, uint32_t t);   // C_t |= S_x
void load_half_rlx_locked(uintptr_t x, uint32_t t);   // F^acq_t |= S_x
void store_half_rel_locked(uintptr_t x, size_t size, uint32_t t); // S_x <- C_t
bool store_half_rlx_locked(uintptr_t x, size_t size, uint32_t t); // S_x <- F^rel or 0; true: released
// A read-modify-write continues the release sequence, so neither changes S_x.
void rmw_half_rel_locked(uintptr_t x, uint32_t t);
bool rmw_half_rlx_locked(uintptr_t x, uint32_t t);

// ---- IR (C++) fences -----------------------------------------------------------

void rule_fence_rel(uint32_t t);    // FenceRel
void rule_fence_acq(uint32_t t);    // FenceAcq
void rule_fence_acqrel(uint32_t t); // FenceAcqRel

// ---- locks and threads -----------------------------------------------------------

void rule_lock(uintptr_t a, uint32_t t);   // Lock
void rule_unlock(uintptr_t a, uint32_t t); // Unlock

// Fork and Join. The caller holds the global lock: both sit inside the thread
// bookkeeping that allocates or looks up u under it. `t` may be UINT32_MAX
// when the parent has no id, in which case u simply starts fresh.
void rule_fork_locked(uint32_t t, uint32_t u);
void rule_join_locked(uint32_t t, uint32_t u);

// Fork for a child PROCESS: ℂ_u ← (the parent's clock as of the fork)[u ↦ 1].
// The parent's own tick was taken in the parent when it forked (thread.cpp);
// `parent_at_fork` is the copy it left for the child.
void rule_fork_process_locked(const VectorClock& parent_at_fork, uint32_t u);

} // namespace csan

#endif
