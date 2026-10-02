// SPDX-License-Identifier: MIT
// Creating, attaching to and configuring the shared segment, and the
// per-process init that binds a machine.
#include "state/segment.h"

#include "repr/arena.h"
#include "base/atomics.h"
#include "base/report.h"
#include "base/spinlock.h"
#include "state/state.h"
#include "repr/vectorclock.h"

#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace csan {

uint32_t allocate_tid_locked() {
    if (g_header->next_tid >= g_threads) {
        std::fprintf(stderr,
                     "CSAN: out of thread slots (%u) -- raise CSAN_THREADS\n",
                     g_threads);
        fflush(stderr);
        return UINT32_MAX;
    }
    return g_header->next_tid++;
}

bool parse_size(const char* s, uint64_t* out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* endp = nullptr;
    unsigned long long v = strtoull(s, &endp, 10);
    if (endp == s) {
        return false;
    }
    uint64_t mult = 1;
    if (*endp == 'K' || *endp == 'k') {
        mult = 1024;
        ++endp;
    } else if (*endp == 'M' || *endp == 'm') {
        mult = 1024ull * 1024;
        ++endp;
    } else if (*endp == 'G' || *endp == 'g') {
        mult = 1024ull * 1024 * 1024;
        ++endp;
    }
    while (*endp == ' ' || *endp == '\t') {
        ++endp;
    }
    if (*endp != '\0') {
        return false;
    }
    *out = static_cast<uint64_t>(v) * mult;
    return true;
}

int open_segment_fd(const char* name) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd >= 0) {
            return fd;
        }
        if (errno == ENOENT || errno == EACCES) {
            usleep(10000);
            continue;
        }
        break;
    }
    std::fprintf(stderr, "csan: cannot open shared segment %s\n", name);
    return -1;
}

// All processes sharing a segment must compute the same layout, so every
// tunable is read once from the environment and otherwise fixed.
uint32_t configured_clock_entries() {
    const char* env = getenv("CSAN_CLOCK_ENTRIES");
    if (env != nullptr && *env != '\0') {
        unsigned long v = strtoul(env, nullptr, 10);
        if (v >= 64 && v <= (1ul << 22)) {
            return static_cast<uint32_t>(v);
        }
    }
    return kDefaultClockEntries;
}

uint32_t configured_threads() {
    const char* env = getenv("CSAN_THREADS");
    if (env != nullptr && *env != '\0') {
        unsigned long v = strtoul(env, nullptr, 10);
        if (v >= 4 && v <= kMaxThreads) {
            return static_cast<uint32_t>(v);
        }
    }
    return kDefaultThreads;
}

uint64_t configured_arena(uint64_t cxl_size) {
    const char* env = getenv("CSAN_ARENA");
    if (env != nullptr && *env != '\0') {
        uint64_t v = 0;
        if (parse_size(env, &v) && v >= (1ull << 20)) {
            return v;
        }
    }
    // Half the pool. The live set is bounded by the state accumulated since
    // each location's last write (Install frees it), so this is generous; if
    // it is ever not, arena_full says so loudly.
    uint64_t want = cxl_size / 2;
    if (want < (4ull << 20)) {
        want = 4ull << 20;
    }
    return want;
}

// CSAN_WATCH_BYTES: shadow reserved for regions the checker watches but did not
// allocate (csan_watch). Zero by default -- a run that watches nothing pays
// nothing -- and part of the layout, so every process must agree on it exactly
// as they must agree on the pool size.
uint64_t configured_watch(void) {
    const char* env = getenv("CSAN_WATCH_BYTES");
    if (env != nullptr && *env != '\0') {
        uint64_t v = 0;
        if (parse_size(env, &v)) {
            return align_up(v, kLineBytes);
        }
    }
    return 0;
}

// CSAN_TRACE: record every event with its call stack, so a finding can be
// reported the way ThreadSanitizer reports one. Off by default.
uint32_t configured_trace_depth(void) {
    const char* on = getenv("CSAN_TRACE");
    if (on == nullptr || *on == '0' || *on == '\0') {
        return 0;
    }
    const char* env = getenv("CSAN_TRACE_DEPTH");
    if (env != nullptr && *env != '\0') {
        uint64_t v = 0;
        if (parse_size(env, &v) && v >= 64) {
            return static_cast<uint32_t>(v);
        }
    }
    return kDefaultTraceDepth;
}

// Rounded up to a power of two: the intern table masks rather than divides.
uint32_t configured_trace_stacks(void) {
    if (configured_trace_depth() == 0) {
        return 0;
    }
    uint64_t want = kDefaultTraceStacks;
    const char* env = getenv("CSAN_TRACE_STACKS");
    if (env != nullptr && *env != '\0') {
        uint64_t v = 0;
        if (parse_size(env, &v) && v >= 256) {
            want = v;
        }
    }
    uint64_t pow2 = 256;
    while (pow2 < want) {
        pow2 <<= 1;
    }
    return static_cast<uint32_t>(pow2);
}

void attach_shared_segment(uint32_t hosts, uint64_t cxl_size) {
    const char* name_env = getenv("CSAN_SHM_NAME");
    bool have_name = name_env != nullptr && *name_env != '\0';
    if (have_name) {
        shm_name() = name_env;
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "/csan_rt_%ld", static_cast<long>(getpid()));
        shm_name() = buf;
        setenv("CSAN_SHM_NAME", shm_name().c_str(), 1);
    }

    uint32_t threads = configured_threads();
    Layout lay = compute_layout(cxl_size, configured_clock_entries(), threads,
                                configured_arena(cxl_size), configured_watch(),
                                configured_trace_depth(), configured_trace_stacks());
    int fd = open_segment_fd(shm_name().c_str());
    if (fd < 0) {
        std::abort();
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        std::perror("csan: fstat");
        std::abort();
    }
    if (static_cast<uint64_t>(st.st_size) != lay.total) {
        if (st.st_size != 0) {
            // A stale file with the wrong size (all processes compute the same
            // deterministic layout from the same environment): reclaim it.
            close(fd);
            shm_unlink(shm_name().c_str());
            fd = open_segment_fd(shm_name().c_str());
            if (fd < 0) {
                std::abort();
            }
        }
        if (ftruncate(fd, static_cast<off_t>(lay.total)) != 0) {
            std::perror("csan: ftruncate");
            std::abort();
        }
    }
    if (ftruncate(fd, static_cast<off_t>(lay.total)) != 0) {
        std::perror("csan: ftruncate");
        std::abort();
    }
    void* m = mmap(nullptr, lay.total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        std::perror("csan: mmap");
        std::abort();
    }

    g_segment = static_cast<uint8_t*>(m);
    g_header = reinterpret_cast<SharedHeader*>(g_segment);
    g_events = lay.trace_depth != 0 ? reinterpret_cast<Event*>(g_segment + lay.events_off)
                                    : nullptr;
    g_stacks = lay.trace_stacks != 0 ? reinterpret_cast<StackEntry*>(g_segment + lay.stacks_off)
                                     : nullptr;
    g_trace_depth = lay.trace_depth;
    g_trace_stacks = lay.trace_stacks;
    g_cells = reinterpret_cast<ByteCell*>(g_segment + lay.cells_off);
    g_lines = reinterpret_cast<LineCell*>(g_segment + lay.lines_off);
    g_prov = lay.trace_depth != 0;
    g_cell_pos = g_prov ? reinterpret_cast<uint32_t*>(g_segment + lay.cellpos_off) : nullptr;
    g_line_pos = g_prov ? reinterpret_cast<uint32_t*>(g_segment + lay.linepos_off) : nullptr;
    g_vc = reinterpret_cast<uint32_t*>(g_segment + lay.vc_off);
    g_frel = g_vc + static_cast<uint64_t>(lay.threads) * lay.threads;
    g_facq = g_frel + static_cast<uint64_t>(lay.threads) * lay.threads;
    g_frel_set = reinterpret_cast<uint8_t*>(g_facq +
                                            static_cast<uint64_t>(lay.threads) * lay.threads);
    g_lock_clocks = g_segment + lay.clocks_off;
    g_sync = reinterpret_cast<SyncEntry*>(
        g_lock_clocks + static_cast<uint64_t>(lay.clock_entries) * lay.clock_stride);
    g_sync_index = reinterpret_cast<uint32_t*>(g_sync + lay.clock_entries);
    g_sync_index_mask = sync_index_slots(lay.clock_entries) - 1;
    g_arena = g_segment + lay.arena_off;
    g_pool = g_segment + lay.pool_off;
    g_threads = lay.threads;
    g_clock_stride = lay.clock_stride;

    uint32_t state = atomic_load_u32(&g_header->init_state);
    if (state != kInitStateReady) {
        if (atomic_cas_u32(&g_header->init_state, kInitStateUninit, kInitStateInitializing)) {
            g_initializer = true;
            // Everything but init_state: clearing it would let an attacher win the CAS too.
            uint64_t flag = offsetof(SharedHeader, init_state);
            std::memset(g_segment, 0, flag);
            std::memset(g_segment + flag + sizeof(uint32_t), 0,
                        lay.total - flag - sizeof(uint32_t));
            g_header->magic = kMagic;
            g_header->version = kVersion;
            g_header->segment_size = lay.total;
            g_header->hosts = hosts;
            g_header->threads = lay.threads;
            g_header->cxl_size = cxl_size;
            g_header->cxl_base = lay.pool_off;
            g_header->watch_size = lay.watch_size;
            g_header->trace_on = lay.trace_depth != 0 ? 1u : 0u;
            g_header->trace_depth = lay.trace_depth;
            g_header->trace_stacks = lay.trace_stacks;
            g_header->watch_bump = cxl_size; // watched shadow starts above the pool's
            g_header->clock_entries = lay.clock_entries;
            g_header->arena_size = lay.arena_size;
            g_header->arena_bump = 8; // offset 0 is the null offset
            // Epochs use kEpochBot as the empty sentinel, but the segment was
            // just zeroed; stamp every cell explicitly.
            for (uint64_t i = 0; i < lay.words; ++i) {
                g_cells[i].wb = kEpochBot;
                g_cells[i].rd = kEpochBot;
            }
            for (uint64_t i = 0; i < lay.lines; ++i) {
                g_lines[i].wl = kEpochBot;
            }
            atomic_store_u32_rel(&g_header->init_state, kInitStateReady);
        } else {
            // Unbounded: clearing and stamping a large shadow takes as long as it takes.
            while (atomic_load_u32_acq(&g_header->init_state) != kInitStateReady) {
                sched_yield();
            }
        }
    }

    if (g_header->magic != kMagic || g_header->version != kVersion ||
        g_header->segment_size != lay.total || g_header->cxl_size != cxl_size ||
        g_header->watch_size != lay.watch_size ||
        g_header->trace_depth != lay.trace_depth ||
        g_header->clock_entries != lay.clock_entries || g_header->threads != lay.threads) {
        std::fprintf(stderr, "csan: shared segment mismatch (magic/version/size)\n");
        std::abort();
    }
    g_attached = true;
}

void csan_ensure_init(uint32_t hosts, uint32_t machine, uint64_t cxl_size) {
    if (g_attached) {
        return;
    }
    std::call_once(g_init_once, [&]() {
        const char* hosts_env = getenv("CSAN_HOSTS");
        if (hosts == 0 && hosts_env != nullptr) {
            hosts = static_cast<uint32_t>(strtoul(hosts_env, nullptr, 10));
        }
        if (hosts == 0 && g_pending_hosts != 0) {
            hosts = g_pending_hosts;
        }
        if (hosts == 0 || hosts > kMaxMachines) {
            hosts = 1;
        }

        const char* machine_env = getenv("CSAN_MACHINE_ID");
        if (machine == UINT32_MAX && machine_env != nullptr) {
            machine = static_cast<uint32_t>(strtoul(machine_env, nullptr, 10));
        }
        if (machine == UINT32_MAX) {
            machine = 0;
        }
        if (machine >= kMaxMachines) {
            machine = 0;
        }

        const char* size_env = getenv("CSAN_TOTAL_MEM");
        if (cxl_size == 0 && size_env != nullptr) {
            uint64_t v = 0;
            if (parse_size(size_env, &v)) {
                cxl_size = v;
            }
        }
        if (cxl_size == 0) {
            cxl_size = g_pending_cxl_size;
        }
        if (cxl_size == 0) {
            cxl_size = kDefaultCxlSize;
        }
        cxl_size = align_up(cxl_size, kLineBytes);
        if (cxl_size < kLineBytes) {
            cxl_size = kLineBytes;
        }

        const char* limit_env = getenv("CSAN_REPORT_LIMIT");
        if (limit_env != nullptr && *limit_env != '\0') {
            g_report_limit = strtoull(limit_env, nullptr, 0);
        }

        const char* trace_env = getenv("CSAN_TRACE_RANGE");
        if (trace_env != nullptr && *trace_env != '\0') {
            char* endp = nullptr;
            unsigned long long lo = strtoull(trace_env, &endp, 0);
            if (endp != nullptr && *endp == ':') {
                unsigned long long hi = strtoull(endp + 1, nullptr, 0);
                if (hi > lo) {
                    g_trace_lo = lo;
                    g_trace_hi = hi;
                    g_trace_on = true;
                }
            }
        }

        attach_shared_segment(hosts, cxl_size);

        spin_lock(&g_header->global_lock);
        g_header->machine_pid[machine] = static_cast<uint32_t>(getpid());
        g_header->machine_registered[machine] = 1;
        g_machine = machine;
        uint32_t tid = allocate_tid_locked();
        if (tid != UINT32_MAX) {
            atomic_store_u32(&g_header->machine_of_thread[tid], machine);
            g_header->machine_main_tid[machine] = tid;
            thread_vc(tid).set(tid, 1);
        }
        spin_unlock(&g_header->global_lock);
        if (tid != UINT32_MAX) {
            t_tid = tid;
        }

        std::atexit([]() {
            std::fprintf(stderr,
                         "CSAN STATS: reads=%" PRIu64 " writes=%" PRIu64 " races=%" PRIu64
                         " overflow=%" PRIu64 " lock_clocks=%u/%u sync_entries=%u/%u"
                         " clock_table_full=%" PRIu64 " arena=%" PRIu64 "/%" PRIu64
                         " arena_full=%" PRIu64 " promote=%" PRIu64 " split=%" PRIu64
                         " rd_shared=%" PRIu64 " epoch_overflow=%" PRIu64 " threads=%u\n",
                         atomic_load_u64(&g_header->read_events),
                         atomic_load_u64(&g_header->write_events),
                         atomic_load_u64(&g_header->race_events),
                         atomic_load_u64(&g_header->overflow_count),
                         atomic_load_u32(&g_header->lock_count), g_header->clock_entries,
                         atomic_load_u32(&g_header->release_count), g_header->clock_entries,
                         atomic_load_u64(&g_header->clock_table_full),
                         atomic_load_u64(&g_header->arena_bump),
                         atomic_load_u64(&g_header->arena_size),
                         atomic_load_u64(&g_header->arena_full),
                         atomic_load_u64(&g_header->promote_count),
                         atomic_load_u64(&g_header->split_count),
                         atomic_load_u64(&g_header->rd_shared_count),
                         atomic_load_u64(&g_header->epoch_overflow), g_header->threads);
            if (g_initializer) {
                shm_unlink(shm_name().c_str());
            }
        });
    });
}

} // namespace csan
