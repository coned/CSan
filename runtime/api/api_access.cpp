// SPDX-License-Identifier: MIT
// Memory events: plain read and write, clwb, clflushopt, sfence, mfence.
//
// Each hook validates -- the runtime is attached, the address is in the pool,
// the thread has an id -- and calls the rule of the same name in rules.h.
#include "csan_runtime.h"
#include "state/layout.h"
#include "base/trace.h"
#include "rules/rules.h"
#include "state/state.h"

using namespace csan;

namespace {

// The thread an event belongs to, or UINT32_MAX when nothing can be checked:
// the runtime is not attached, or this thread could get no id.
uint32_t acting_thread() {
    // See api_sync.cpp: a hook re-entered from a fault inside another hook's
    // access records nothing rather than waiting for the lock it holds.
    return g_attached && !inside_hook() ? __csan_thread_id() : UINT32_MAX;
}

} // namespace

extern "C" {

// Read(x,t) over [addr, addr+n). Addresses outside the pool are not CXL memory.
void __csan_read(const void* addr, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, n)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvRead, a, static_cast<uint32_t>(n), t);
        rule_read(a, n, t);
    }
}

// Write(x,t) over [addr, addr+n).
void __csan_write(const void* addr, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, n)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvWrite, a, static_cast<uint32_t>(n), t);
        rule_write(a, n, t);
    }
}

void __csan_memcpy(void* dst, const void* src, size_t n) {
    __csan_read(src, n);
    __csan_write(dst, n);
}

// WriteBack over the lines [addr, addr+n) touches.
void __csan_wb(const void* addr, size_t n, uint32_t src) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, 1)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvWriteBack, a, static_cast<uint32_t>(n), t, 0, src);
        rule_writeback(a, n, t);
    }
}

// Flush over the lines [addr, addr+n) touches.
void __csan_flush(const void* addr, size_t n, uint32_t src) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, 1)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        // Recorded first, so the state the rule changes is credited to this
        // flush; what it acquired is known only after, and is added then.
        uint32_t ref = trace_record(kEvFlush, a, static_cast<uint32_t>(n), t, 0, src);
        trace_set_from(ref, rule_flush(a, n, t));
    }
}

void __csan_sfence(void) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvSFence, 0, 0, t);
        rule_sfence(t);
    }
}

void __csan_mfence(void) {
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvMFence, 0, 0, t);
        rule_mfence(t);
    }
}

} // extern "C"
