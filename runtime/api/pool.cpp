// SPDX-License-Identifier: MIT
// Pool allocation, region registration and watched regions.
#include "api/pool.h"
#include "base/atomics.h"
#include "state/layout.h"
#include "state/segment.h"
#include "base/spinlock.h"
#include "csan_runtime.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace csan {

void add_range_locked(uintptr_t base, uintptr_t end) {
    for (uint32_t i = 0; i < kMaxRanges; ++i) {
        SharedRange& r = g_header->ranges[i];
        if (!r.in_use) {
            r.base = base;
            r.end = end;
            r.in_use = 1;
            if (i >= g_header->range_count) {
                g_header->range_count = i + 1;
            }
            return;
        }
    }
}

// Returns the end of the range that was removed, or 0 if there was none.
uintptr_t remove_range_locked(uintptr_t base) {
    for (uint32_t i = 0; i < g_header->range_count; ++i) {
        SharedRange& r = g_header->ranges[i];
        if (r.in_use && r.base == base) {
            r.in_use = 0;
            return static_cast<uintptr_t>(r.end);
        }
    }
    return 0;
}

// Set or clear kHwCcBit on every line [base, base+size) touches.
//
// Allocations are kAlign = 64 = kLineBytes aligned and padded, so a region
// always starts on a line boundary and no line ever carries two regions --
// marking by line can never speak for memory the caller did not name.
//
// No line lock: this runs at registration, before the region is handed to the
// program, and the flag is a byte of its own that the access path only reads.
void mark_hwcc_lines(uintptr_t base, size_t size, bool on) {
    uintptr_t end = base + static_cast<uintptr_t>(size);
    for (uintptr_t line = base & ~(kLineBytes - 1); line < end; line += kLineBytes) {
        LineCell* lc = line_cell_at(line);
        uint8_t f = __atomic_load_n(&lc->flags, __ATOMIC_RELAXED);
        uint8_t next = on ? static_cast<uint8_t>(f | kHwCcBit)
                          : static_cast<uint8_t>(f & ~kHwCcBit);
        __atomic_store_n(&lc->flags, next, __ATOMIC_RELAXED);
    }
}

} // namespace csan

using namespace csan;

extern "C" {

void* csan_alloc(size_t size) {
    if (!g_attached) {
        return nullptr;
    }
    if (size == 0) {
        size = 1;
    }
    uint64_t n = align_up(static_cast<uint64_t>(size), kAlign);
    spin_lock(&g_header->global_lock);
    uint64_t off = g_header->bump;
    if (off + n > g_header->cxl_size) {
        spin_unlock(&g_header->global_lock);
        return nullptr;
    }
    g_header->bump = off + n;
    uintptr_t base = reinterpret_cast<uintptr_t>(g_pool) + off;
    add_range_locked(base, base + n);
    spin_unlock(&g_header->global_lock);
    if (getenv("CSAN_DEBUG_ALLOC") != nullptr) {
        std::fprintf(stderr, "CXL ALLOC off=0x%06" PRIx64 " size=0x%06" PRIx64 "\n",
                     static_cast<uint64_t>(off), n);
    }
    return g_pool + off;
}

// The region every machine knows by `id`: the first caller carves it out of
// the pool and the rest get the same one back. Backed by the root table, so an
// id is a root index and the two share the kMaxRoots slots.
void* csan_shared_region(uint32_t id, size_t size) {
    if (!g_attached || id >= kMaxRoots) {
        return nullptr;
    }
    if (size == 0) {
        size = 1;
    }
    uint64_t n = align_up(static_cast<uint64_t>(size), kAlign);
    spin_lock(&g_header->global_lock);
    uint64_t stored = g_header->roots[id];
    if (stored == 0) {
        uint64_t off = g_header->bump;
        if (off + n <= g_header->cxl_size) {
            g_header->bump = off + n;
            uintptr_t base = reinterpret_cast<uintptr_t>(g_pool) + off;
            add_range_locked(base, base + n);
            stored = off + 1; // roots hold offset + 1; 0 means unset
            g_header->roots[id] = stored;
        }
    } else {
        // An existing region must be at least as large as asked for.
        uint64_t off = stored - 1;
        for (uint32_t i = 0; i < g_header->range_count; ++i) {
            SharedRange& r = g_header->ranges[i];
            if (r.in_use && r.base == reinterpret_cast<uintptr_t>(g_pool) + off &&
                r.end - r.base < n) {
                stored = 0;
            }
        }
    }
    spin_unlock(&g_header->global_lock);
    return stored != 0 ? g_pool + (stored - 1) : nullptr;
}

void* csan_realloc(void* ptr, size_t size) {
    uint64_t old_size = 0;
    if (ptr != nullptr && g_attached) {
        spin_lock(&g_header->global_lock);
        uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
        for (uint32_t i = 0; i < g_header->range_count; ++i) {
            SharedRange& r = g_header->ranges[i];
            if (r.in_use && r.base == base) {
                old_size = r.end - r.base;
                break;
            }
        }
        spin_unlock(&g_header->global_lock);
    }
    void* p = csan_alloc(size);
    if (p != nullptr && ptr != nullptr && old_size > 0) {
        std::memcpy(p, ptr, std::min<uint64_t>(old_size, size));
    }
    csan_free(ptr);
    return p;
}

void csan_free(void* ptr) {
    if (!g_attached || ptr == nullptr) {
        return;
    }
    spin_lock(&g_header->global_lock);
    remove_range_locked(reinterpret_cast<uintptr_t>(ptr));
    spin_unlock(&g_header->global_lock);
}

void csan_register_scc_range(void* base, size_t size) {
    __csan_register_scc_range(base, size);
}

void csan_register_hwcc_range(void* base, size_t size) {
    __csan_register_hwcc_range(base, size);
}

void csan_register_range(void* base, size_t size) {
    __csan_register_range(base, size);
}

void csan_unregister_range(void* base) {
    __csan_unregister_range(base);
}

// ---- support for compatibility layers ---------------------------------------
//
// A compatibility layer stands in for another system's pool API on top of this
// runtime. These are the runtime internals such a layer is allowed to reach.

void* __csan_pool_base(void) {
    return g_attached ? g_pool : nullptr;
}

uint64_t __csan_pool_size(void) {
    return g_attached ? g_header->cxl_size : 0;
}

void __csan_register_range(void* base, size_t size) {
    if (!g_attached || base == nullptr || size == 0) {
        return;
    }
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (!cxl_contains(b, size)) {
        std::fprintf(stderr,
                     "CSAN: refusing to register range outside the managed pool "
                     "(0x%012" PRIxPTR ", %zu bytes)\n",
                     b, size);
        return;
    }
    spin_lock(&g_header->global_lock);
    add_range_locked(b, b + static_cast<uintptr_t>(size));
    spin_unlock(&g_header->global_lock);
}

// The two kinds of CXL memory a program can declare.
//
// Software-coherent is the default and needs no call: the segment starts zeroed
// and csan_alloc clears the flag.
//
// Hardware-coherent opts out of the visibility checks only. The checker records
// and reports nothing for those bytes, but a lock or atomic living there still
// carries its happens-before edges, since those order accesses to memory that
// IS checked (rules_access, rules_visibility).
static void register_range_as(void* base, size_t size, bool hwcc) {
    __csan_register_range(base, size);
    if (!g_attached || base == nullptr || size == 0) {
        return;
    }
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (!cxl_contains(b, size)) {
        return; // __csan_register_range has already complained
    }
    mark_hwcc_lines(b, size, hwcc);
}

void __csan_register_scc_range(void* base, size_t size) {
    register_range_as(base, size, /*hwcc=*/false);
}

void __csan_register_hwcc_range(void* base, size_t size) {
    register_range_as(base, size, /*hwcc=*/true);
}

void __csan_unregister_range(void* base) {
    if (!g_attached || base == nullptr) {
        return;
    }
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    spin_lock(&g_header->global_lock);
    uintptr_t end = remove_range_locked(b);
    spin_unlock(&g_header->global_lock);
    // Reset the lines to the default so a later registration starts clean.
    if (end > b) {
        mark_hwcc_lines(b, static_cast<size_t>(end - b), /*on=*/false);
    }
}

int __csan_is_cxl(const void* addr, size_t n) {
    if (!g_attached || n == 0) {
        return 0;
    }
    return cxl_contains(reinterpret_cast<uintptr_t>(addr), n) ? 1 : 0;
}

// Watch memory the checker did not allocate. `id` names the region; every
// machine that maps it registers it under the same id, and the state is keyed
// by (id, offset) so each machine may map it wherever it likes.
void csan_watch(uint32_t id, void* base, size_t size) {
    if (base == nullptr || size == 0) {
        return;
    }
    csan_ensure_init(0, UINT32_MAX, 0);
    if (!g_attached) {
        return;
    }
    uint64_t n = align_up(static_cast<uint64_t>(size), kLineBytes);
    uintptr_t b = reinterpret_cast<uintptr_t>(base);
    // A line is the unit of every visibility obligation, so a region that began
    // mid-line would share its first line with memory nobody watched.
    if ((b & (kLineBytes - 1)) != 0) {
        std::fprintf(stderr, "CSAN: watched region %u is not line-aligned (0x%012" PRIxPTR ")\n",
                     id, b);
        return;
    }

    spin_lock(&g_header->global_lock);
    uint32_t slot = kMaxWatchRegions;
    for (uint32_t i = 0; i < g_header->watch_count; ++i) {
        if (g_header->watch[i].in_use && g_header->watch[i].id == id) {
            slot = i;
            break;
        }
    }
    if (slot == kMaxWatchRegions && g_header->watch_count < kMaxWatchRegions &&
        g_header->watch_bump + n <= g_header->cxl_size + g_header->watch_size) {
        // The first machine to name this region fixes its size and its place in
        // the shadow; the others only record where they mapped it.
        slot = g_header->watch_count++;
        g_header->watch[slot].id = id;
        g_header->watch[slot].size = n;
        g_header->watch[slot].shadow_off = g_header->watch_bump;
        g_header->watch[slot].in_use = 1;
        g_header->watch_bump += n;
    }
    uint64_t agreed = slot != kMaxWatchRegions ? g_header->watch[slot].size : 0;
    spin_unlock(&g_header->global_lock);

    if (slot == kMaxWatchRegions) {
        // Never leave a region silently unchecked.
        std::fprintf(stderr,
                     "CSAN: no room to watch region %u (%llu bytes); raise CSAN_WATCH_BYTES\n",
                     id, static_cast<unsigned long long>(n));
        atomic_fetch_add_u64(&g_header->overflow_count, 1);
        return;
    }
    if (agreed != n) {
        std::fprintf(stderr,
                     "CSAN: region %u was watched as %llu bytes, now %llu; every machine "
                     "must name the same region with the same size\n",
                     id, static_cast<unsigned long long>(agreed),
                     static_cast<unsigned long long>(n));
        return;
    }
    g_watch_base[slot] = b;
    ++g_region_gen; // every thread here re-resolves before trusting a cached region
}

} // extern "C"
