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
bool g_report_off = false;
bool g_report_once = true;

bool trace_in_range(uintptr_t addr) {
    if (g_pool == nullptr) {
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
    "data-race-wr",               "read-without-byte-acquire",
    "retired",                    "data-race-rw",
    "write-without-line-acquire", "atomic-write-without-line-acquire", "data-race-ww",
    "write-without-publish",
};

const char* race_kind_name(uint32_t kind) {
    return kind < CSAN_RACE_KIND_COUNT ? kRaceKindNames[kind] : "?";
}

// One writer per counter slot (the reporting thread), so no shared cache line.
static void count_race(uint32_t kind, uint32_t tid) {
    if (tid >= kMaxThreads) {
        return;
    }
    ThreadCounts& c = g_header->counts[tid];
    count_add(&c.races, 1);
    if (kind < CSAN_RACE_KIND_COUNT) {
        count_add(&c.race_kind[kind], 1);
    }
}

void report_race(uint32_t kind, uintptr_t addr, uint32_t tid, Epoch other, uintptr_t other_addr) {
    count_race(kind, tid);
    if (g_report_off) {
        return;
    }
    // Past the print limit only the counts change: no text, no global lock,
    // and no shared index once the limit is known to be reached.
    if (g_report_limit != 0 && atomic_load_u32(&g_header->report_head) > g_report_limit) {
        return;
    }
    uint64_t seen = atomic_fetch_add_u32(&g_header->report_head, 1);
    bool print = g_report_limit == 0 || seen < g_report_limit;
    if (!print) {
        if (seen == g_report_limit) {
            spin_lock(&g_header->global_lock);
            std::fprintf(stderr,
                         "CSAN: report limit %" PRIu64 " reached; further findings are counted"
                         " but not printed (set CSAN_REPORT_LIMIT, 0 for unlimited)\n",
                         g_report_limit);
            fflush(stderr);
            spin_unlock(&g_header->global_lock);
        }
        return;
    }
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
    uint32_t head = static_cast<uint32_t>(seen % 64);
    std::snprintf(g_header->report_lines[head], sizeof(g_header->report_lines[head]), "%s", buf);
    // Print under the lock so reports never interleave.
    if (tracing()) {
        trace_print_finding(stderr, kind, addr, 1, tid,
                            other != kEpochBot ? epoch_tid(other) : UINT32_MAX,
                            other_addr != 0 ? other_addr : addr);
    } else {
        fputs(buf, stderr);
    }
    fflush(stderr);
    spin_unlock(&g_header->global_lock);
}

} // namespace csan
