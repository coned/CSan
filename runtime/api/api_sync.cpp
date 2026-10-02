// SPDX-License-Identifier: MIT
// Synchronization events: the atomic operations the pass replaces, IR fences,
// recognized locks, and the manual release/acquire hooks.
//
// Each hook decodes its flags into the rule to fire and calls it. The rules are
// in rules_sync.cpp; the flag word is described in rawatomic.h.
#include "csan_runtime.h"
#include "state/layout.h"
#include "base/rawatomic.h"
#include "base/report.h"
#include "rules/rules.h"
#include "base/spinlock.h"
#include "base/trace.h"
#include "state/state.h"
#include "repr/vectorclock.h"

using namespace csan;

namespace {

uint32_t acting_thread() {
    // A hook re-entered from a fault inside another hook's access has no
    // thread to check against: it performs the access and records nothing.
    return g_attached && !inside_hook() ? __csan_thread_id() : UINT32_MAX;
}

// Whether an atomic operation also runs the shadow half: only where the
// location has a shadow cell. Atomics are routinely used on memory outside the
// pool -- a process-local handshake, for instance -- and those have none.
bool shadow_for(uintptr_t a, uint32_t size) {
    return cxl_contains(a, size);
}

} // namespace

extern "C" {

// ---- atomic operations: load, store, read-modify-write, compare-exchange ------
//
// A thread with no id still performs the operation, unchecked: it has no clock
// to record against.

uint64_t __csan_atomic_load(const void* addr, uint32_t size, uint32_t flags) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return raw_atomic_load(addr, size);
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    bool shadow = shadow_for(a, size);
    if (shadow) {
        trace_record(kEvRead, a, size, t);
    }
    if ((flags & kAtomicAcquire) != 0) {
        return rule_load_acq(addr, size, t, shadow);
    }
    return rule_load_rlx(addr, size, t, shadow);
}

void __csan_atomic_store(void* addr, uint64_t val, uint32_t size, uint32_t flags) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        raw_atomic_store(addr, val, size);
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    bool shadow = shadow_for(a, size);
    if (shadow) {
        trace_record(kEvWrite, a, size, t);
    }
    if ((flags & kAtomicRelease) != 0) {
        rule_store_rel(addr, val, size, t, shadow, (flags & kAtomicMFenced) != 0);
    } else {
        rule_store_rlx(addr, val, size, t, shadow);
    }
}

uint64_t __csan_atomic_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t flags) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return raw_atomic_rmw(addr, val, size, op);
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    bool shadow = shadow_for(a, size);
    // An RMW writes, so it is recorded as the write a later report looks for.
    if (shadow) {
        trace_record(kEvWrite, a, size, t);
    }
    return rule_rmw(addr, val, size, op, t, (flags & kAtomicAcquire) != 0,
                    (flags & kAtomicRelease) != 0, shadow, (flags & kAtomicMFenced) != 0);
}

uint64_t __csan_atomic_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size,
                           uint32_t flags) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return raw_atomic_cas(addr, expected, desired, size);
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    bool shadow = shadow_for(a, size);
    if (shadow) {
        trace_record(kEvWrite, a, size, t);
    }
    return rule_cas(addr, expected, desired, size, t, (flags & kAtomicAcquire) != 0,
                    (flags & kAtomicRelease) != 0, (flags & kAtomicFailAcquire) != 0, shadow,
                    (flags & kAtomicMFenced) != 0);
}

// ---- IR fences ----------------------------------------------------------------

void __csan_fence_release(void) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        rule_fence_rel(t);
    }
}

void __csan_fence_acquire(void) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        rule_fence_acq(t);
    }
}

void __csan_fence_acqrel(void) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        rule_fence_acqrel(t);
    }
}

// ---- locks --------------------------------------------------------------------

void __csan_lock(const void* lock) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        rule_lock(reinterpret_cast<uintptr_t>(lock), t);
    }
}

void __csan_unlock(const void* lock) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        rule_unlock(reinterpret_cast<uintptr_t>(lock), t);
    }
}

// ---- manual release / acquire -------------------------------------------------
//
// For call sites the pass does not replace: the caller has already performed
// the access itself, so these apply only the clock effect of the store or load
// half, and there is no shadow half. The store forms differ as StoreRel and
// RmwRel do: a release STORE heads the only surviving sequence at the address,
// a release READ-MODIFY-WRITE extends the existing ones.

void __csan_release(const void* addr) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    spin_lock(&g_header->global_lock);
    store_half_rel_locked(a, 1, t);
    if (trace_hit(a)) {
        trace_event("RELEASE", a, t, "store");
    }
    spin_unlock(&g_header->global_lock);
    tick(t);
}

void __csan_release_rmw(const void* addr) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    spin_lock(&g_header->global_lock);
    rmw_half_rel_locked(a, t);
    if (trace_hit(a)) {
        trace_event("REL-RMW", a, t, "merged");
    }
    spin_unlock(&g_header->global_lock);
    tick(t);
}

void __csan_acquire(const void* addr) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    spin_lock(&g_header->global_lock);
    load_half_acq_locked(a, t);
    if (trace_hit(a)) {
        trace_event("ACQUIRE", a, t, "merged");
    }
    spin_unlock(&g_header->global_lock);
}

} // extern "C"
