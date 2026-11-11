// SPDX-License-Identifier: MIT
#include "repr/arena.h"

#include "base/atomics.h"
#include "state/state.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace csan {

// The first failure goes through the shared counter and prints; later ones
// count in the thread's own slot, off the shared line.
static uint32_t arena_fail() {
    uint64_t* shared = &g_header->arena_full;
    if (t_tid < kMaxThreads && atomic_load_u64(shared) != 0) {
        count_add(&g_header->counts[t_tid].arena_full, 1);
        return 0;
    }
    if (atomic_fetch_add_u64(shared, 1) == 0) {
        std::fprintf(stderr,
                     "CSAN: shadow arena full (%" PRIu64 " bytes) -- publication "
                     "and read state can no longer be recorded, so FALSE RACES are "
                     "expected from here on. Raise CSAN_ARENA.\n",
                     g_header->arena_size);
        fflush(stderr);
    }
    return 0;
}

// Atomic stores, not memset: a seqlock reader holding a stale offset may
// still read the block (ReadList, split array).
static void zero_block(uint32_t off, uint64_t sz) {
    uint32_t* w = reinterpret_cast<uint32_t*>(g_arena + off);
    for (uint64_t i = 0; i < sz / sizeof(uint32_t); ++i) {
        atomic_store_u32(&w[i], 0);
    }
}

uint32_t arena_alloc(uint32_t cls) {
    uint64_t sz = arena_class_size(cls);
    // The bump pointer only grows: past the end, then no free block, is a
    // moment the locked path below fails too.
    uint64_t lim = std::min<uint64_t>(g_header->arena_size, UINT32_MAX);
    if (atomic_load_u64(&g_header->arena_bump) + sz > lim &&
        atomic_load_u32(&g_header->arena_free[cls]) == 0) {
        return arena_fail();
    }
    spin_lock(&g_header->arena_lock);
    uint32_t head = g_header->arena_free[cls];
    if (head != 0) {
        atomic_store_u32(&g_header->arena_free[cls],
                         atomic_load_u32(reinterpret_cast<uint32_t*>(g_arena + head)));
        spin_unlock(&g_header->arena_lock);
        zero_block(head, sz);
        return head;
    }
    uint64_t off = g_header->arena_bump;
    if (off + sz > g_header->arena_size || off + sz > UINT32_MAX) {
        spin_unlock(&g_header->arena_lock);
        return arena_fail();
    }
    __atomic_store_n(&g_header->arena_bump, off + sz, __ATOMIC_RELAXED);
    spin_unlock(&g_header->arena_lock);
    zero_block(static_cast<uint32_t>(off), sz);
    return static_cast<uint32_t>(off);
}

void count_overflow() {
    if (t_tid < kMaxThreads) {
        count_add(&g_header->counts[t_tid].overflow, 1);
    } else {
        atomic_fetch_add_u64(&g_header->overflow_count, 1);
    }
}

uint64_t overflow_total() {
    return atomic_load_u64(&g_header->overflow_count) + count_total(&ThreadCounts::overflow);
}

void arena_free(uint32_t cls, uint32_t off) {
    if (off == 0) {
        return;
    }
    spin_lock(&g_header->arena_lock);
    atomic_store_u32(reinterpret_cast<uint32_t*>(g_arena + off), g_header->arena_free[cls]);
    atomic_store_u32(&g_header->arena_free[cls], off);
    spin_unlock(&g_header->arena_lock);
}

} // namespace csan
