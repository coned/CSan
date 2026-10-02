// SPDX-License-Identifier: MIT
#include "repr/arena.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace csan {

uint32_t arena_alloc(uint32_t cls) {
    uint64_t sz = arena_class_size(cls);
    spin_lock(&g_header->arena_lock);
    uint32_t head = g_header->arena_free[cls];
    if (head != 0) {
        g_header->arena_free[cls] = *reinterpret_cast<uint32_t*>(g_arena + head);
        spin_unlock(&g_header->arena_lock);
        std::memset(g_arena + head, 0, sz);
        return head;
    }
    uint64_t off = g_header->arena_bump;
    if (off + sz > g_header->arena_size || off + sz > UINT32_MAX) {
        spin_unlock(&g_header->arena_lock);
        if (atomic_fetch_add_u64(&g_header->arena_full, 1) == 0) {
            std::fprintf(stderr,
                         "CSAN: shadow arena full (%" PRIu64 " bytes) -- publication "
                         "and read state can no longer be recorded, so FALSE RACES are "
                         "expected from here on. Raise CSAN_ARENA.\n",
                         g_header->arena_size);
            fflush(stderr);
        }
        return 0;
    }
    g_header->arena_bump = off + sz;
    spin_unlock(&g_header->arena_lock);
    std::memset(g_arena + off, 0, sz);
    return static_cast<uint32_t>(off);
}

void arena_free(uint32_t cls, uint32_t off) {
    if (off == 0) {
        return;
    }
    spin_lock(&g_header->arena_lock);
    *reinterpret_cast<uint32_t*>(g_arena + off) = g_header->arena_free[cls];
    g_header->arena_free[cls] = off;
    spin_unlock(&g_header->arena_lock);
}


} // namespace csan
