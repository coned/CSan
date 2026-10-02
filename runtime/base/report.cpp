// SPDX-License-Identifier: MIT
// Emitting findings, and the address-range trace (CSAN_TRACE_RANGE).
#include "base/report.h"

#include "base/atomics.h"
#include "base/spinlock.h"
#include "base/trace.h"
#include "repr/vectorclock.h"

#include <cinttypes>
#include <cstdio>

namespace csan {

uint64_t g_trace_lo = 0;
uint64_t g_trace_hi = 0;
bool g_trace_on = false;
uint64_t g_report_limit = 100;

bool trace_hit(uintptr_t addr) {
    if (!g_trace_on || g_pool == nullptr) {
        return false;
    }
    uint64_t off = shadow_offset(addr);
    return off >= g_trace_lo && off < g_trace_hi;
}

void trace_event(const char* what, uintptr_t addr, uint32_t tid, const char* note) {
    uint64_t off = shadow_offset(addr);
    char vcbuf[128];
    int n = 0;
    for (uint32_t i = 0; i < 12 && n < static_cast<int>(sizeof(vcbuf)) - 12; ++i) {
        n += snprintf(vcbuf + n, sizeof(vcbuf) - n, "%u,", vc_get(tid, i));
    }
    std::fprintf(stderr, "CXL TRACE %-8s pool+0x%06llx tid=%u machine=%u clock=%u vc=[%s] %s\n",
                 what, static_cast<unsigned long long>(off), tid, machine_of(tid),
                 vc_get(tid, tid), vcbuf, note != nullptr ? note : "");
    fflush(stderr);
}

// Indexed by CSAN_RACE_*; the order there is the order here.
static const char* const kRaceKindNames[CSAN_RACE_KIND_COUNT] = {
    "read-before-hb-write",       "read-without-byte-acquire",
    "write-before-hb-line-write", "write-after-read",
    "write-without-line-acquire", "atomic-write-without-line-acquire", "data-race",
};

const char* race_kind_name(uint32_t kind) {
    return kind < CSAN_RACE_KIND_COUNT ? kRaceKindNames[kind] : "?";
}

void report_race(uint32_t kind, uintptr_t addr, uint32_t tid, Epoch other) {
    const char* name = race_kind_name(kind);
    char buf[512];
    uint64_t off = cxl_contains(addr, 1) ? shadow_offset(addr) : UINT64_MAX;
    if (other != kEpochBot) {
        uint32_t ot = epoch_tid(other);
        snprintf(buf, sizeof(buf),
                 "CSAN RACE: %s at pool+0x%06" PRIx64 " (0x%012" PRIxPTR
                 ") reader thread %u machine %u writer thread %u machine %u"
                 " wb_clock=%u reader_vc_writer=%u\n",
                 name, off, addr, tid, machine_of(tid), ot, machine_of(ot), epoch_clock(other),
                 vc_get(tid, ot));
    } else {
        snprintf(buf, sizeof(buf),
                 "CSAN RACE: %s at pool+0x%06" PRIx64 " (0x%012" PRIxPTR
                 ") thread %u machine %u\n",
                 name, off, addr, tid, machine_of(tid));
    }
    spin_lock(&g_header->global_lock);
    // report_head counts every finding ever emitted, across machines, so the
    // limit caps the whole run's output and not one process's share of it.
    uint64_t seen = g_header->report_head;
    bool print = g_report_limit == 0 || seen < g_report_limit;
    uint32_t head = g_header->report_head % 64;
    std::snprintf(g_header->report_lines[head], sizeof(g_header->report_lines[head]), "%s", buf);
    ++g_header->report_head;
    // Print inside the critical section so concurrent reports never interleave.
    if (!print) {
        if (seen == g_report_limit) {
            std::fprintf(stderr,
                         "CSAN: report limit %" PRIu64 " reached; further findings are counted"
                         " but not printed (set CSAN_REPORT_LIMIT, 0 for unlimited)\n",
                         g_report_limit);
            fflush(stderr);
        }
    } else if (tracing()) {
        trace_print_finding(stderr, kind, addr, 1, tid,
                            other != kEpochBot ? epoch_tid(other) : UINT32_MAX);
    } else {
        fputs(buf, stderr);
    }
    if (print) {
        fflush(stderr);
    }
    spin_unlock(&g_header->global_lock);
    atomic_fetch_add_u64(&g_header->race_events, 1);
    if (kind < CSAN_RACE_KIND_COUNT) {
        atomic_fetch_add_u64(&g_header->race_kind[kind], 1);
    }
}


} // namespace csan
