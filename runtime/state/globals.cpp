// SPDX-License-Identifier: MIT
// Definitions of the globals declared in layout.h and state.h.
#include "state/layout.h"
#include "state/state.h"

namespace csan {

// ---- process-local pointers into the mapping -------------------------------

SharedHeader* g_header = nullptr;
uint8_t* g_segment = nullptr;
uint8_t* g_pool = nullptr;
Region g_regions[kMaxRegions] = {};
uint32_t g_region_count = 0;
Event* g_events = nullptr;
StackEntry* g_stacks = nullptr;
uint32_t g_trace_depth = 0;
uint32_t g_trace_stacks = 0;
ByteCell* g_cells = nullptr;
LineCell* g_lines = nullptr;
bool g_prov = false;
uint32_t* g_cell_pos = nullptr;
uint32_t* g_vc = nullptr;   // thread clocks: g_vc[tid * g_threads + other]
uint32_t* g_frel = nullptr; // release-fence clocks, same shape
uint32_t* g_facq = nullptr; // acquire-fence clocks, same shape
uint8_t* g_frel_set = nullptr; // whether the release-fence clock is set, per thread
SyncStripe* g_sync_stripes = nullptr;
SyncEntry* g_sync = nullptr;
uint32_t g_sync_slots = 0;
uint8_t* g_arena = nullptr;
uint32_t g_threads = kDefaultThreads;
uint32_t g_machine = UINT32_MAX;
uint32_t g_memo_gen = 0;
bool g_attached = false;
bool g_initializer = false;
uint32_t g_pending_hosts = 0;
uint64_t g_pending_cxl_size = 0;
thread_local uint32_t t_tid = UINT32_MAX;
thread_local uint32_t t_hook_depth = 0;
thread_local std::vector<uint32_t> t_fork_clock;
thread_local uint32_t t_fork_parent = UINT32_MAX;
std::once_flag g_init_once;

// Leaked on purpose: used from program static constructors and atexit, outside
// this file's initialization lifetime.
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
