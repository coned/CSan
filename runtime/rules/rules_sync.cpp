// SPDX-License-Identifier: MIT
// The synchronization rules: atomic loads, stores, read-modify-writes and
// compare-exchange, IR fences, and locks, fork and join.
//
// An atomic operation records its clock effect under the global lock and
// performs the access itself OUTSIDE that critical section, ordered so that no
// thread can observe the value without also observing the clock: a clock a
// value publishes is recorded BEFORE the value (StoreRel/StoreRlx, the write
// half of an Rmw), and a clock a value consumes is joined AFTER it
// (LoadAcq/LoadRlx, the read half of an Rmw).
//
// The access must not happen under the lock: a program that demand-maps its
// CXL memory from a SIGSEGV handler would fault inside it and deadlock. A
// releasing compare-exchange cannot be split this way and takes t_hook_depth
// instead (state.h).
//
// 𝕊_x crosses machines. The visibility premises do not apply to an atomic
// access, but still apply to every plain access, including one to a byte an
// atomic wrote.
#include "rules/rules.h"

#include "base/atomics.h"
#include "base/rawatomic.h"
#include "base/report.h"
#include "base/spinlock.h"
#include "repr/sync.h"

#include <immintrin.h>

namespace csan {

// A real clwb + sfence so an atomic store reaches CXL memory when 𝕊_x records
// it. Not an event: it never counts as the publication half of any chain.
static void push_atomic_write(const void* addr) {
    if (!cxl_contains(reinterpret_cast<uintptr_t>(addr), 1)) {
        return; // not CXL memory: the hardware handles it
    }
    _mm_clwb(const_cast<void*>(addr));
    _mm_sfence();
}

// The MFence effect an MFENCED atomic carries: MF_{t,E} on this thread's
// relays. MFenced means the x86 lowering is a full barrier -- every
// read-modify-write at every ordering, and a seq_cst store.
//
// Runs BEFORE the rule's shadow half and at the epoch before any tick it takes,
// so the records it finishes are ordered ahead of the access.
//
// Takes no lock of its own but does take line locks, so the caller must NOT
// hold the global lock.
static void mfence_effect(uint32_t t, bool mfenced) {
    if (mfenced) {
        mf_state(t, now_epoch(t));
    }
}

// The key of location a in the 𝕊_x and 𝕃_a tables. A location inside the
// pool is shared CXL memory, so its key is its pool OFFSET -- exec'd machines
// map the pool at different addresses. A location outside the pool is
// process-private memory that fork can place at the same address in every
// machine, so the machine id is folded in: two machines' unrelated mutexes at
// one address must not exchange clocks. Bit 63 separates the two kinds.
static uint64_t sync_key(uintptr_t a, uint32_t t) {
    if (cxl_contains(a, 1)) {
        return shadow_offset(a);
    }
    return (1ull << 63) | (static_cast<uint64_t>(a) ^ ((static_cast<uint64_t>(machine_of(t)) + 1) << 48));
}

// ---- the load and store halves ---------------------------------------------------
//
// 𝕊_x is one clock per location: the clock at the head of the release sequence
// currently live there. Only a read-modify-write continues a sequence (C++20);
// any other store ends it.

// LoadAcq's clock effect: ℂ_t ← ℂ_t ⊔ 𝕊_x. A release on another machine is
// joined like any other: 𝕊_x is not partitioned by machine.
void load_half_acq_locked(uintptr_t x, uint32_t t) {
    thread_vc(t).merge(sync_s(sync_key(x, t)));
}

// LoadRlx's clock effect: 𝔽^acq_t ← 𝔽^acq_t ⊔ 𝕊_x, staged for a later acquire fence.
void load_half_rlx_locked(uintptr_t x, uint32_t t) {
    facq_vc(t).merge(sync_s(sync_key(x, t)));
}

// StoreRel's clock effect: 𝕊_x ← ℂ_t. The store heads a new release sequence,
// so whatever was live at x is gone.
void store_half_rel_locked(uintptr_t x, size_t size, uint32_t t) {
    uint64_t k = sync_key(x, t);
    for (uint64_t i = 1; i < size; ++i) {
        sync_s_clear(k + i); // the other bytes it covers carry nothing
    }
    sync_s_set(k, thread_vc(t));
}

// StoreRlx's clock effect: 𝕊_x ← 𝔽^rel_t if a release fence is staged --
// fence-release plus a relaxed store IS a release operation and heads a
// sequence -- and 𝕊_x ← 0 otherwise, since a plain store ends the sequence
// even when the storing thread is the one that began it. Returns whether the
// store released something, which is when the thread's clock must advance.
bool store_half_rlx_locked(uintptr_t x, size_t size, uint32_t t) {
    uint64_t k = sync_key(x, t);
    for (uint64_t i = 1; i < size; ++i) {
        sync_s_clear(k + i);
    }
    if (!frel_is_set(t)) {
        sync_s_clear(k);
        return false;
    }
    sync_s_set(k, frel_vc(t));
    return true;
}

// The read-modify-write store halves. A read-modify-write CONTINUES the release
// sequence at x rather than ending it, so 𝕊_x is never cleared here.
//
// A RELEASING one also heads a sequence of its own, so it JOINS its clock into
// 𝕊_x: an acquire reading its value synchronizes with it, and through the
// sequence with the release before it.
void rmw_half_rel_locked(uintptr_t x, uint32_t t) {
    sync_s_merge(sync_key(x, t), thread_vc(t));
}

// A relaxed read-modify-write publishes only what a staged release fence
// captured: fence-release then a relaxed RMW is a release operation.
bool rmw_half_rlx_locked(uintptr_t x, uint32_t t) {
    if (!frel_is_set(t)) {
        return false;
    }
    sync_s_merge(sync_key(x, t), frel_vc(t));
    return true;
}

// ---- atomic loads ----------------------------------------------------------------
//
// The premise -- atomic_x = ⊤ ∨ W^B_x ⊑ 𝔻 -- and ReadVis are checked by the
// shadow half AFTER the join, as the rules state them in terms of 𝔻. A load
// never advances ℂ_t[t].

static uint64_t atomic_load(const void* addr, uint32_t size, uint32_t t, bool acquire,
                            bool shadow) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    // The value first, the join after it: 𝕊_x read here is the one live at or
    // after the moment the value was read, so it covers whatever release this
    // load could have observed. Joining first could take a release whose value
    // this load did not read, which is an edge that does not exist.
    uint64_t v = raw_atomic_load(addr, size);
    spin_lock(&g_header->global_lock);
    if (acquire) {
        load_half_acq_locked(x, t);
    } else {
        load_half_rlx_locked(x, t);
    }
    spin_unlock(&g_header->global_lock);
    if (shadow) {
        read_range(x, size, t, /*atomic=*/true);
    }
    return v;
}

// LoadRlx:  𝔽^acq_t ← 𝔽^acq_t ⊔ 𝕊_x;  ℝ_x[t] ← ℂ_t[t];  atomic_x ← ⊤
uint64_t rule_load_rlx(const void* addr, uint32_t size, uint32_t t, bool shadow) {
    return atomic_load(addr, size, t, /*acquire=*/false, shadow);
}

// LoadAcq:  𝔻 = ℂ_t ⊔ 𝕊_x;  ℂ_t ← 𝔻;  ℝ_x[t] ← 𝔻[t];  atomic_x ← ⊤
uint64_t rule_load_acq(const void* addr, uint32_t size, uint32_t t, bool shadow) {
    return atomic_load(addr, size, t, /*acquire=*/true, shadow);
}

// ---- atomic stores ---------------------------------------------------------------
//
// Install first, then the clock effect, then the value -- all before another
// thread can observe it. Install is what resets the relays and the read clock;
// the store half is what ends the old release sequences.

static void atomic_store(void* addr, uint64_t val, uint32_t size, uint32_t t, bool release,
                         bool shadow, bool mfenced) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    mfence_effect(t, mfenced);
    if (shadow) {
        write_range(x, size, t, /*atomic=*/true);
    }
    spin_lock(&g_header->global_lock);
    bool released;
    if (release) {
        store_half_rel_locked(x, size, t);
        released = true;
    } else {
        released = store_half_rlx_locked(x, size, t);
    }
    spin_unlock(&g_header->global_lock);
    // The clock first, the value after it: a thread that observes this value
    // observes a 𝕊_x that already holds what the store released.
    raw_atomic_store(addr, val, size);
    push_atomic_write(addr);
    if (released) {
        tick(t);
    }
}

// StoreRlx:  Install_{x,now(t)};  𝕊_x ← 𝕍_x(t) ⊔ 𝔽^rel_t;  𝕍_x ← ∅[t ↦ 𝕍_x(t)];
//            atomic_x ← ⊤;  ℂ_t[t] ← ℂ_t[t] + (𝔽^rel_t ≠ ⊥ ? 1 : 0)
void rule_store_rlx(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow) {
    // A relaxed or release store is a plain `mov` on x86 and is never MFenced;
    // the seq_cst store that IS reaches rule_store_rel, since it also releases.
    atomic_store(addr, val, size, t, /*release=*/false, shadow, /*mfenced=*/false);
}

// StoreRel:  Install_{x,now(t)};  𝕊_x ← ℂ_t;  𝕍_x ← ∅[t ↦ ℂ_t];  atomic_x ← ⊤;  ℂ_t[t] ← ℂ_t[t]+1
void rule_store_rel(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow,
                    bool mfenced) {
    atomic_store(addr, val, size, t, /*release=*/true, shadow, mfenced);
}

// ---- read-modify-write and compare-exchange -----------------------------------
//
// A read-modify-write is the atomic load behaviour followed by the atomic
// store behaviour: the read half decides whether 𝕊_x is joined into ℂ_t or
// staged in 𝔽^acq_t, the write half whether 𝕊_x and 𝕍_x are extended.

struct RmwOutcome {
    bool released; // the write half released: the clock must advance
};

static RmwOutcome rmw_halves_locked(uintptr_t x, uint32_t t, bool acquire, bool release,
                                    bool wrote) {
    RmwOutcome o{false};
    if (acquire) {
        load_half_acq_locked(x, t);
    } else {
        load_half_rlx_locked(x, t);
    }
    if (wrote) {
        if (release) {
            rmw_half_rel_locked(x, t);
            o.released = true;
        } else {
            o.released = rmw_half_rlx_locked(x, t);
        }
    }
    return o;
}

// Everything after the critical section: the shadow half (read, and if the
// operation wrote, the write with its Install), and the tick.
static void rmw_finish(uintptr_t x, uint32_t size, uint32_t t, RmwOutcome o, bool wrote,
                       bool shadow) {
    if (shadow) {
        read_range(x, size, t, /*atomic=*/true);
        if (wrote) {
            write_range(x, size, t, /*atomic=*/true);
        }
    }
    if (o.released) {
        tick(t);
    }
}

// RmwRlx:     Install;  𝔽^acq_t ⊔= 𝕊_x;  𝕊_x ⊔= 𝔽^rel_t;  atomic_x ← ⊤
// RmwAcq:     𝔻 = ℂ_t ⊔ 𝕊_x;  ℂ_t ← 𝔻;  Install;  𝕊_x ⊔= 𝔽^rel_t;  atomic_x ← ⊤
// RmwRel:     Install;  𝔽^acq_t ⊔= 𝕊_x;  𝕊_x ⊔= ℂ_t;  𝕍_x[t] ← ℂ_t;  atomic_x ← ⊤;  tick
// RmwAcqRel:  𝔻 = ℂ_t ⊔ 𝕊_x;  ℂ_t ← 𝔻;  Install;  𝕊_x ⊔= 𝔻;  𝕍_x[t] ← 𝔻;  atomic_x ← ⊤;  tick
//
// RmwRlx and RmwAcq tick only when 𝔽^rel_t ≠ ⊥: then they released what the
// fence captured, and a write before that release and a write after it must
// not share an epoch -- the same condition as StoreRlx.
uint64_t rule_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t t,
                  bool acquire, bool release, bool shadow, bool mfenced) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    mfence_effect(t, mfenced);
    // A read-modify-write always writes, so which halves apply is known before
    // it runs and the two are taken in separate critical sections around it:
    // the write half before, so a consumer of the value cannot miss the clock;
    // the read half after, so the join covers whatever release the value came
    // from. Both are merges into 𝕊_x, so splitting them changes no result.
    RmwOutcome o{false};
    spin_lock(&g_header->global_lock);
    if (release) {
        rmw_half_rel_locked(x, t);
        o.released = true;
    } else {
        o.released = rmw_half_rlx_locked(x, t);
    }
    spin_unlock(&g_header->global_lock);

    uint64_t old = raw_atomic_rmw(addr, val, size, op);
    push_atomic_write(addr);

    spin_lock(&g_header->global_lock);
    if (acquire) {
        load_half_acq_locked(x, t);
    } else {
        load_half_rlx_locked(x, t);
    }
    spin_unlock(&g_header->global_lock);
    rmw_finish(x, size, t, o, /*wrote=*/true, shadow);
    return old;
}

// CasOk:    the comparison succeeded  ⟹  the Rmw rule at the success ordering
// CasFail:  the comparison failed     ⟹  the load rule at the failure ordering:
//           no Install, no change to 𝕊_x or 𝕍_x, no tick, and WriteVis not required.
uint64_t rule_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size, uint32_t t,
                  bool ok_acquire, bool ok_release, bool fail_acquire, bool shadow,
                  bool mfenced) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    mfence_effect(t, mfenced); // `lock cmpxchg` fences whether or not it swaps
    if (!ok_release && !frel_is_set(t)) {
        // Nothing this compare-exchange could publish, so the swap runs outside
        // the lock and its read half joins after it, exactly as a load's does.
        uint64_t old = raw_atomic_cas(addr, expected, desired, size);
        bool swapped = old == expected;
        if (swapped) {
            push_atomic_write(addr);
        }
        spin_lock(&g_header->global_lock);
        RmwOutcome o = rmw_halves_locked(x, t, swapped ? ok_acquire : fail_acquire,
                                         /*release=*/false, /*wrote=*/swapped);
        spin_unlock(&g_header->global_lock);
        rmw_finish(x, size, t, o, /*wrote=*/swapped, shadow);
        return old;
    }
    // A RELEASING compare-exchange is the one operation that cannot be split:
    // whether it writes -- and so whether it releases at all -- is known only
    // once it has run, so its clock cannot be recorded ahead of the value it
    // publishes, and recording it afterwards would let a consumer read that
    // value with the release missing from 𝕊_x. The access therefore stays
    // inside the critical section, and a hook it re-enters sees t_hook_depth
    // and returns without instrumenting rather than waiting for this lock.
    HookDepth nested;
    spin_lock(&g_header->global_lock);
    uint64_t old = raw_atomic_cas(addr, expected, desired, size);
    bool swapped = old == expected;
    if (swapped) {
        push_atomic_write(addr);
    }
    RmwOutcome o = rmw_halves_locked(x, t, swapped ? ok_acquire : fail_acquire, ok_release,
                                     /*wrote=*/swapped);
    spin_unlock(&g_header->global_lock);
    nested.release();
    rmw_finish(x, size, t, o, /*wrote=*/swapped, shadow);
    return old;
}

// ---- IR fences --------------------------------------------------------------------
//
// An LLVM `fence` affects clocks only. On x86 an acquire, release or
// acquire-release fence generates no instruction, so it orders nothing in the
// machine and in particular cannot order a clwb; publication and acquisition
// are the sfence/mfence intrinsics' job (rule_sfence, rule_mfence).

// FenceRel:  𝔽^rel_t ← ℂ_t;  ℂ_t[t] ← ℂ_t[t]+1
void rule_fence_rel(uint32_t t) {
    spin_lock(&g_header->global_lock);
    frel_vc(t).copy_from(thread_vc(t));
    frel_mark(t);
    spin_unlock(&g_header->global_lock);
    tick(t);
}

// FenceAcq:  𝔻 = ℂ_t ⊔ 𝔽^acq_t;  ℂ_t ← 𝔻;  ℂ_t[t] ← 𝔻[t]+1
void rule_fence_acq(uint32_t t) {
    spin_lock(&g_header->global_lock);
    thread_vc(t).merge(facq_vc(t));
    spin_unlock(&g_header->global_lock);
    tick(t);
}

// FenceAcqRel:  𝔻 = ℂ_t ⊔ 𝔽^acq_t;  ℂ_t ← 𝔻;  𝔽^rel_t ← 𝔻;  ℂ_t[t] ← 𝔻[t]+1
void rule_fence_acqrel(uint32_t t) {
    spin_lock(&g_header->global_lock);
    thread_vc(t).merge(facq_vc(t));
    frel_vc(t).copy_from(thread_vc(t));
    frel_mark(t);
    spin_unlock(&g_header->global_lock);
    tick(t);
}

// ---- locks and threads ------------------------------------------------------------
//
// These establish happens-before only. They never write back, flush or fence.

// Lock:  𝔻 = ℂ_t ⊔ 𝕃_a;  ℂ_t ← 𝔻;  ℂ_t[t] ← 𝔻[t]+1
void rule_lock(uintptr_t a, uint32_t t) {
    spin_lock(&g_header->global_lock);
    ClockEntry* e =
        find_or_add_clock_entry(g_lock_clocks, &g_header->lock_count, sync_key(a, t));
    if (e != nullptr) {
        thread_vc(t).merge(clock_entry_vc(e));
    }
    tick(t);
    spin_unlock(&g_header->global_lock);
}

// Unlock:  𝕃_a ← ℂ_t;  ℂ_t[t] ← ℂ_t[t]+1
void rule_unlock(uintptr_t a, uint32_t t) {
    spin_lock(&g_header->global_lock);
    ClockEntry* e =
        find_or_add_clock_entry(g_lock_clocks, &g_header->lock_count, sync_key(a, t));
    if (e != nullptr) {
        clock_entry_vc(e).copy_from(thread_vc(t));
    }
    tick(t);
    spin_unlock(&g_header->global_lock);
}

// Fork:  u fresh  ⟹  ℂ_u ← ℂ_t[u ↦ 1];  ℂ_t[t] ← ℂ_t[t]+1
void rule_fork_locked(uint32_t t, uint32_t u) {
    VectorClock cu = thread_vc(u);
    if (t != UINT32_MAX) {
        cu.merge(thread_vc(t));
    }
    cu.set(u, 1);
    if (t != UINT32_MAX) {
        tick(t);
    }
}

// Fork, the child-process half: the child's main thread u gets the forking
// thread's clock as it was at the fork. Taking the parent's LIVE row here would
// order everything the parent did between fork() and this call before the
// child, which is a happens-before edge that does not exist.
void rule_fork_process_locked(const VectorClock& parent_at_fork, uint32_t u) {
    VectorClock cu = thread_vc(u);
    cu.merge(parent_at_fork);
    cu.set(u, 1);
}

// Join:  𝔻 = ℂ_t ⊔ ℂ_u;  ℂ_t ← 𝔻;  ℂ_t[t] ← 𝔻[t]+1
void rule_join_locked(uint32_t t, uint32_t u) {
    thread_vc(t).merge(thread_vc(u));
    tick(t);
}

} // namespace csan
