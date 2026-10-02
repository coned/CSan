// SPDX-License-Identifier: MIT
// Reading results back: the report ring and the counters.
#include "csan_runtime.h"
#include "base/atomics.h"
#include "base/report.h"
#include "state/layout.h"
#include "base/spinlock.h"
#include "state/state.h"


using namespace csan;

extern "C" {

uint64_t csan_race_events(void) {
    return __csan_race_events();
}

const char* __csan_report(void) {
    report_buffer().clear();
    if (g_attached) {
        spin_lock(&g_header->global_lock);
        uint32_t total = std::min<uint32_t>(g_header->report_head, 64);
        for (uint32_t i = 0; i < total; ++i) {
            report_buffer() += g_header->report_lines[i];
            report_buffer() += '\n';
        }
        spin_unlock(&g_header->global_lock);
    }
    return report_buffer().c_str();
}

uint64_t __csan_read_events(void) {
    return g_attached ? atomic_load_u64(&g_header->read_events) : 0;
}

uint64_t __csan_write_events(void) {
    return g_attached ? atomic_load_u64(&g_header->write_events) : 0;
}

uint64_t __csan_race_events(void) {
    return g_attached ? atomic_load_u64(&g_header->race_events) : 0;
}

uint64_t __csan_overflow_count(void) {
    return g_attached ? atomic_load_u64(&g_header->overflow_count) : 0;
}

uint64_t __csan_race_kind_count(uint32_t kind) {
    if (!g_attached || kind >= CSAN_RACE_KIND_COUNT) {
        return 0;
    }
    return atomic_load_u64(&g_header->race_kind[kind]);
}

const char* __csan_race_kind_name(uint32_t kind) {
    return race_kind_name(kind);
}

} // extern "C"
