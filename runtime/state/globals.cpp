// SPDX-License-Identifier: MIT
// Definitions of the runtime's global state, declared in layout.h (the
// segment-backed pointers) and state.h (everything process-local).
#include "state/layout.h"
#include "state/state.h"

namespace csan {

// ---- process-local pointers into the mapping -------------------------------

SharedHeader* g_header = nullptr;
uint8_t* g_segment = nullptr;
uint8_t* g_pool = nullptr;
uintptr_t g_watch_base[kMaxWatchRegions] = {};

// This thread's last resolved region. Empty until the first access places one,
// and an empty cache never matches: lo = ~0 and hi = 0.
thread_local uintptr_t t_region_lo = UINTPTR_MAX;
thread_local uintptr_t t_region_hi = 0;
thread_local uint64_t t_region_bias = 0;
thread_local uint32_t t_region_gen = 0;
uint32_t g_region_gen = 1; // never 0, so an untouched cache never matches

bool region_resolve(uintptr_t a) {
    if (g_pool == nullptr) {
        return false;
    }
    t_region_gen = g_region_gen;
    uintptr_t base = reinterpret_cast<uintptr_t>(g_pool);
    if (a >= base && a < base + g_header->cxl_size) {
        t_region_lo = base;
        t_region_hi = base + g_header->cxl_size;
        t_region_bias = 0 - static_cast<uint64_t>(base); // shadow index = a - base
        return true;
    }
    uint32_t i = watch_index(a, 1);
    if (i == kMaxWatchRegions) {
        return false;
    }
    t_region_lo = g_watch_base[i];
    t_region_hi = t_region_lo + g_header->watch[i].size;
    t_region_bias = g_header->watch[i].shadow_off - static_cast<uint64_t>(t_region_lo);
    return true;
}
Event* g_events = nullptr;
StackEntry* g_stacks = nullptr;
uint32_t g_trace_depth = 0;
uint32_t g_trace_stacks = 0;
ByteCell* g_cells = nullptr;
LineCell* g_lines = nullptr;
bool g_prov = false;
uint32_t* g_cell_pos = nullptr;
uint32_t* g_line_pos = nullptr;
uint32_t* g_vc = nullptr;   // thread clocks: g_vc[tid * g_threads + other]
uint32_t* g_frel = nullptr; // release-fence clocks, same shape
uint32_t* g_facq = nullptr; // acquire-fence clocks, same shape
uint8_t* g_frel_set = nullptr; // whether the release-fence clock is set, per thread
uint8_t* g_lock_clocks = nullptr;
SyncEntry* g_sync = nullptr;
uint32_t* g_sync_index = nullptr;
uint64_t g_sync_index_mask = 0;
uint8_t* g_arena = nullptr;
uint32_t g_threads = kDefaultThreads;
uint64_t g_clock_stride = 0;
uint32_t g_machine = UINT32_MAX;
bool g_attached = false;
bool g_initializer = false;
uint32_t g_pending_hosts = 0;
uint64_t g_pending_cxl_size = 0;
thread_local uint32_t t_tid = UINT32_MAX;
thread_local uint32_t t_hook_depth = 0;
thread_local std::vector<uint32_t> t_fork_clock;
thread_local uint32_t t_fork_parent = UINT32_MAX;
std::once_flag g_init_once;

// Built on first use and never destroyed: a program's static constructor can
// attach before this file's dynamic initializers run, and its atexit can run
// after they would be destroyed.
std::unordered_map<pthread_t, uint32_t>& thread_ids() {
    static auto* m = new std::unordered_map<pthread_t, uint32_t>();
    return *m;
}

std::string& shm_name() {
    static auto* s = new std::string();
    return *s;
}

std::string& report_buffer() {
    static auto* s = new std::string();
    return *s;
}

std::vector<Pending>& pending_queue(uint32_t t, uint32_t kind) {
    static auto* q = new std::vector<Pending>[kMaxThreads][4];
    return q[t][kind];
}

} // namespace csan
