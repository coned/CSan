// SPDX-License-Identifier: MIT
// Synchronization hooks: atomics, IR fences, locks, manual release/acquire.
// Rules are in rules_sync.cpp; flags in rawatomic.h.
#include "csan_runtime.h"
#include "state/layout.h"
#include "base/rawatomic.h"
#include "base/report.h"
#include "rules/rules.h"
#include "base/trace.h"
#include "state/state.h"
#include "repr/vectorclock.h"

using namespace csan;

namespace {

uint32_t acting_thread() {
    // A hook re-entered from inside another hook records nothing.
    if (!g_attached || inside_hook()) {
        return UINT32_MAX;
    }
    return t_tid != UINT32_MAX ? t_tid : __csan_thread_id();
}

// Only pool memory has shadow; atomics are often used outside it.
bool shadow_for(uintptr_t a, uint32_t size) {
    return cxl_contains(a, size);
}

} // namespace

extern "C" {

// ---- atomics ----
// A thread with no id still performs the operation, unchecked.

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

// ---- IR fences ----

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

// ---- locks ----

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

// ---- manual release / acquire ----
// The caller did the access; these apply only its clock effect. A release
// store replaces the sequences at the address; a release RMW extends them.

void __csan_release(const void* addr) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    store_half(a, 1, t, /*release=*/true);
    if (trace_hit(a)) {
        trace_event("RELEASE", a, t, "store");
    }
    tick(t);
}

void __csan_release_rmw(const void* addr) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    rmw_half(a, t, /*release=*/true);
    if (trace_hit(a)) {
        trace_event("REL-RMW", a, t, "merged");
    }
    tick(t);
}

void __csan_acquire(const void* addr) {
    uint32_t t = acting_thread();
    if (t == UINT32_MAX) {
        return;
    }
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    load_half(a, t, /*acquire=*/true);
    if (trace_hit(a)) {
        trace_event("ACQUIRE", a, t, "merged");
    }
}

} // extern "C"
