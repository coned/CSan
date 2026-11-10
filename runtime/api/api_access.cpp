// SPDX-License-Identifier: MIT
// Memory event hooks: read, write, clwb, clflushopt, sfence, mfence. Each
// validates and calls the matching rule in rules.h.
#include "csan_runtime.h"
#include "state/layout.h"
#include "base/trace.h"
#include "rules/rules.h"
#include "state/state.h"

using namespace csan;

namespace {

// UINT32_MAX when nothing can be checked. A hook re-entered from inside
// another hook records nothing rather than wait on a lock it holds.
uint32_t acting_thread() {
    return g_attached && !inside_hook() ? __csan_thread_id() : UINT32_MAX;
}

} // namespace

extern "C" {

void __csan_read(const void* addr, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, n)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvRead, a, static_cast<uint32_t>(n), t);
        rule_load(a, n, t);
    }
}

void __csan_write(const void* addr, size_t n) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, n)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        trace_record(kEvWrite, a, static_cast<uint32_t>(n), t);
        rule_store(a, n, t);
    }
}

void __csan_memcpy(void* dst, const void* src, size_t n) {
    __csan_read(src, n);
    __csan_write(dst, n);
}

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

void __csan_flush(const void* addr, size_t n, uint32_t src) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    if (!g_attached || n == 0 || !cxl_contains(a, 1)) {
        return;
    }
    uint32_t t = acting_thread();
    if (t != UINT32_MAX) {
        // Record before the rule so its changes are credited to this flush.
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
