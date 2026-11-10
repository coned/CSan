// SPDX-License-Identifier: MIT
// Synchronization rules: atomics, IR fences, locks, fork and join.
//
// Clock effects run under the location's stripe lock; the access runs outside
// it (a SIGSEGV handler that demand-maps CXL memory would deadlock inside).
// Published clocks are recorded before the value, consumed ones joined after.
// A releasing CAS can't be split and uses t_hook_depth instead (state.h).
// C_t, F^rel_t, F^acq_t have one writer, t, so need no lock.
#include "rules/rules.h"

#include "base/rawatomic.h"
#include "repr/shadow.h"
#include "repr/sync.h"

#include <algorithm>
#include <immintrin.h>

namespace csan {

// Real clwb + sfence so the store reaches CXL memory. Not an event: never
// counts toward publication.
static void push_atomic_write(const void* addr) {
    if (!cxl_contains(reinterpret_cast<uintptr_t>(addr), 1)) {
        return;
    }
    _mm_clwb(const_cast<void*>(addr));
    _mm_sfence();
}

// The MFence effect, for an atomic lowered to a full barrier. Takes line locks.
static void mfence_effect(uint32_t t, bool mfenced) {
    if (mfenced) {
        apply_mfence(t, now_epoch(t));
    }
}

// CXL: the shadow offset (machines map it at different addresses). Elsewhere
// two machines' objects can share an address, so fold in the machine; bit 63
// separates the two kinds.
static uint64_t sync_key(uintptr_t a, uint32_t t) {
    if (cxl_contains(a, 1)) {
        return shadow_offset(a);
    }
    return (1ull << 63) |
           (static_cast<uint64_t>(a) ^ ((static_cast<uint64_t>(machine_of(t)) + 1) << 48));
}

// Flags the cell first so a later plain write ends the sequence.
static uint64_t release_key(uintptr_t x, uint32_t t) {
    uint64_t k = sync_key(x, t);
    if (cxl_contains(x, 1)) {
        mark_release_site(k);
    }
    return k;
}

// F^rel_t if a release fence is staged, else the zero clock.
static VectorClock staged_release(uint32_t t) {
    return frel_is_set(t) ? frel_vc(t) : VectorClock();
}

// ---- load and store halves ----
// S_x: the release sequence head's clock. Only an RMW continues a sequence.

// LoadAcq: C_t ⊔= S_x.  LoadRlx: F^acq_t ⊔= S_x, for a later acquire fence.
void load_half(uintptr_t x, uint32_t t, bool acquire) {
    sync_join(sync_key(x, t), acquire ? thread_vc(t) : facq_vc(t));
}

// StoreRel: S_x <- C_t.  StoreRlx: S_x <- F^rel_t, or 0 with no fence staged.
// Other covered bytes carry no sequence. True when it released.
bool store_half(uintptr_t x, size_t size, uint32_t t, bool release) {
    VectorClock c = release ? thread_vc(t) : staged_release(t);
    uint64_t k = c.valid() ? release_key(x, t) : sync_key(x, t);
    uint64_t word_end = (k / kWordBytes + 1) * kWordBytes;
    for (uint64_t i = word_end; i < k + size; ++i) {
        SyncRef(i).erase(); // unaligned: bytes in the next word
    }
    SyncRef s(k);
    for (uint64_t i = k + 1; i < std::min(k + size, word_end); ++i) {
        s.erase_byte(i);
    }
    if (!c.valid()) {
        s.erase();
        return false;
    }
    s.clock_or_add().copy_from(c);
    return true;
}

// RmwRel: S_x ⊔= C_t.  RmwRlx: S_x ⊔= F^rel_t. True when it released.
bool rmw_half(uintptr_t x, uint32_t t, bool release) {
    VectorClock c = release ? thread_vc(t) : staged_release(t);
    if (!c.valid()) {
        return false;
    }
    SyncRef(release_key(x, t)).clock_or_add().merge(c);
    return true;
}

// ---- atomic loads ----

// LoadRlx / LoadAcq; shadow: R_x[t] <- C_t[t], atomic_x <- ⊤.
// Value first, then join, so the join covers the value's release.
static uint64_t atomic_load(const void* addr, uint32_t size, uint32_t t, bool acquire,
                            bool shadow) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    uint64_t v = raw_atomic_load(addr, size);
    load_half(x, t, acquire);
    if (shadow) {
        load_range(x, size, t, /*atomic=*/true);
    }
    return v;
}

uint64_t rule_load_rlx(const void* addr, uint32_t size, uint32_t t, bool shadow) {
    return atomic_load(addr, size, t, /*acquire=*/false, shadow);
}

uint64_t rule_load_acq(const void* addr, uint32_t size, uint32_t t, bool shadow) {
    return atomic_load(addr, size, t, /*acquire=*/true, shadow);
}

// ---- atomic stores ----

// Install, clock, then value: whoever sees the value sees S_x already set.
static void atomic_store(void* addr, uint64_t val, uint32_t size, uint32_t t, bool release,
                         bool shadow, bool mfenced) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    mfence_effect(t, mfenced);
    if (shadow) {
        store_range(x, size, t, /*atomic=*/true);
    }
    bool released = store_half(x, size, t, release);
    raw_atomic_store(addr, val, size);
    push_atomic_write(addr);
    if (released) {
        tick(t);
    }
}

// A relaxed store is a plain `mov`: never MFenced.
void rule_store_rlx(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow) {
    atomic_store(addr, val, size, t, /*release=*/false, shadow, /*mfenced=*/false);
}

void rule_store_rel(void* addr, uint64_t val, uint32_t size, uint32_t t, bool shadow,
                    bool mfenced) {
    atomic_store(addr, val, size, t, /*release=*/true, shadow, mfenced);
}

// ---- read-modify-write and compare-exchange ----

static void rmw_finish(uintptr_t x, uint32_t size, uint32_t t, bool released, bool wrote,
                       bool shadow) {
    if (shadow) {
        load_range(x, size, t, /*atomic=*/true);
        if (wrote) {
            store_range(x, size, t, /*atomic=*/true);
        }
    }
    if (released) {
        tick(t);
    }
}

// Write half before the operation, so a consumer of the value can't miss the
// clock; read half after.
uint64_t rule_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t t,
                  bool acquire, bool release, bool shadow, bool mfenced) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    mfence_effect(t, mfenced);
    bool released = rmw_half(x, t, release);
    uint64_t old = raw_atomic_rmw(addr, val, size, op);
    push_atomic_write(addr);
    load_half(x, t, acquire);
    rmw_finish(x, size, t, released, /*wrote=*/true, shadow);
    return old;
}

// CasFail: no Install, no change to S_x, no tick.
uint64_t rule_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size, uint32_t t,
                  bool ok_acquire, bool ok_release, bool fail_acquire, bool shadow,
                  bool mfenced) {
    uintptr_t x = reinterpret_cast<uintptr_t>(addr);
    mfence_effect(t, mfenced); // `lock cmpxchg` fences whether or not it swaps
    VectorClock rel = ok_release ? thread_vc(t) : staged_release(t);
    if (!rel.valid()) {
        // Nothing to publish: swap, then join like a load.
        uint64_t old = raw_atomic_cas(addr, expected, desired, size);
        bool swapped = old == expected;
        if (swapped) {
            push_atomic_write(addr);
        }
        load_half(x, t, swapped ? ok_acquire : fail_acquire);
        rmw_finish(x, size, t, /*released=*/false, swapped, shadow);
        return old;
    }
    // Whether it releases is known only after it runs, so the access stays in
    // the critical section; a re-entered hook sees t_hook_depth and won't wait.
    uint64_t k = release_key(x, t);
    HookDepth nested;
    uint64_t old;
    bool swapped;
    {
        SyncRef s(k);
        old = raw_atomic_cas(addr, expected, desired, size);
        swapped = old == expected;
        if (swapped) {
            push_atomic_write(addr);
        }
        bool acquire = swapped ? ok_acquire : fail_acquire;
        (acquire ? thread_vc(t) : facq_vc(t)).merge(s.clock());
        if (swapped) {
            s.clock_or_add().merge(rel);
        }
    }
    nested.release();
    rmw_finish(x, size, t, swapped, swapped, shadow);
    return old;
}

// ---- IR fences: clocks only; they order no clwb on x86 ----

// FenceRel: F^rel_t <- C_t; tick.
void rule_fence_rel(uint32_t t) {
    frel_vc(t).copy_from(thread_vc(t));
    frel_mark(t);
    tick(t);
}

// FenceAcq: C_t ⊔= F^acq_t; tick.
void rule_fence_acq(uint32_t t) {
    thread_vc(t).merge(facq_vc(t));
    tick(t);
}

// FenceAcqRel: C_t ⊔= F^acq_t; F^rel_t <- C_t; tick.
void rule_fence_acqrel(uint32_t t) {
    thread_vc(t).merge(facq_vc(t));
    frel_vc(t).copy_from(thread_vc(t));
    frel_mark(t);
    tick(t);
}

// ---- locks and threads ----

// Lock: C_t ⊔= L_a; tick.
void rule_lock(uintptr_t a, uint32_t t) {
    sync_join(lock_key(sync_key(a, t)), thread_vc(t));
    tick(t);
}

// Unlock: L_a <- C_t; tick.
void rule_unlock(uintptr_t a, uint32_t t) {
    {
        SyncRef s(lock_key(sync_key(a, t)));
        s.clock_or_add().copy_from(thread_vc(t));
    }
    tick(t);
}

// Fork: u fresh ⟹ C_u <- C_t[u ↦ 1]; tick t.
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

// Uses the parent's clock as of the fork; its live row has moved on.
void rule_fork_process_locked(const VectorClock& parent_at_fork, uint32_t u) {
    VectorClock cu = thread_vc(u);
    cu.merge(parent_at_fork);
    cu.set(u, 1);
}

// Join: C_t ⊔= C_u; tick.
void rule_join_locked(uint32_t t, uint32_t u) {
    thread_vc(t).merge(thread_vc(u));
    tick(t);
}

} // namespace csan
