// SPDX-License-Identifier: MIT
#include "repr/sync.h"

#include "base/spinlock.h"

#include <cstdio>

namespace csan {

// ---- 𝕃_a, the lock clock table ----------------------------------------------

ClockEntry* clock_entry(uint8_t* table, uint32_t i) {
    return reinterpret_cast<ClockEntry*>(table + static_cast<uint64_t>(i) * g_clock_stride);
}

VectorClock clock_entry_vc(ClockEntry* e) {
    return VectorClock(
        reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(e) + sizeof(ClockEntry)));
}

ClockEntry* find_clock_entry(uint8_t* table, uint32_t count, uint64_t key) {
    for (uint32_t i = 0; i < count; ++i) {
        ClockEntry* e = clock_entry(table, i);
        if (e->in_use && e->key == key) {
            return e;
        }
    }
    return nullptr;
}

ClockEntry* find_or_add_clock_entry(uint8_t* table, uint32_t* count, uint64_t key) {
    ClockEntry* e = find_clock_entry(table, *count, key);
    if (e != nullptr) {
        return e;
    }
    // Lock clocks are never retired (nothing knows when an address stops being
    // a lock), so the table grows with the distinct lock addresses a run uses.
    if (*count >= g_header->clock_entries) {
        if (atomic_fetch_add_u64(&g_header->clock_table_full, 1) == 0) {
            std::fprintf(stderr,
                         "CSAN: lock clock table full (%u entries) -- "
                         "happens-before can no longer be tracked for locks at new "
                         "addresses, so FALSE REPORTS are expected from here on. "
                         "Raise CSAN_CLOCK_ENTRIES.\n",
                         g_header->clock_entries);
            fflush(stderr);
        }
        return nullptr;
    }
    e = clock_entry(table, (*count)++);
    e->key = key;
    e->in_use = 1;
    clock_entry_vc(e).clear();
    return e;
}

// ---- 𝕊_x --------------------------------------------------------------------

namespace {

// Open-addressed hash index over the table. A slot holds entry index + 1, 0
// for empty, kTomb for a deleted entry a probe must step over.
constexpr uint32_t kTomb = UINT32_MAX;

uint64_t slot_of(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key & g_sync_index_mask;
}

// The index slot naming `key`, or null.
uint32_t* find_slot(uint64_t key) {
    for (uint64_t i = slot_of(key);; i = (i + 1) & g_sync_index_mask) {
        uint32_t v = g_sync_index[i];
        if (v == 0) {
            return nullptr;
        }
        if (v != kTomb && g_sync[v - 1].key == key) {
            return &g_sync_index[i];
        }
    }
}

void index_insert(uint32_t entry) {
    for (uint64_t i = slot_of(g_sync[entry].key);; i = (i + 1) & g_sync_index_mask) {
        uint32_t v = g_sync_index[i];
        if (v == 0 || v == kTomb) {
            if (v == kTomb) {
                --g_header->sync_tombstones;
            }
            g_sync_index[i] = entry + 1;
            return;
        }
    }
}

// Tombstones only lengthen probes; past a quarter of the index, start over.
void index_rebuild() {
    for (uint64_t i = 0; i <= g_sync_index_mask; ++i) {
        g_sync_index[i] = 0;
    }
    g_header->sync_tombstones = 0;
    for (uint32_t i = 0; i < g_header->release_count; ++i) {
        if (g_sync[i].in_use) {
            index_insert(i);
        }
    }
}

SyncEntry* find(uint64_t key) {
    uint32_t* slot = find_slot(key);
    return slot != nullptr ? &g_sync[*slot - 1] : nullptr;
}

// The entry for `key`, taking a free slot when there is none. Null when the
// table is full, which is a correctness cliff and so is loud.
SyncEntry* find_or_add(uint64_t key) {
    SyncEntry* e = find(key);
    if (e != nullptr) {
        return e;
    }
    uint32_t idx;
    if (g_header->sync_free_head != 0) {
        idx = g_header->sync_free_head - 1;
        g_header->sync_free_head = static_cast<uint32_t>(g_sync[idx].key);
    } else {
        if (g_header->release_count >= g_header->clock_entries) {
            if (atomic_fetch_add_u64(&g_header->clock_table_full, 1) == 0) {
                std::fprintf(stderr,
                             "CSAN: synchronization table full (%u entries) -- "
                             "happens-before can no longer be tracked for new atomic "
                             "addresses, so FALSE RACES are expected from here on. "
                             "Raise CSAN_CLOCK_ENTRIES.\n",
                             g_header->clock_entries);
                fflush(stderr);
            }
            return nullptr;
        }
        idx = g_header->release_count++;
    }
    e = &g_sync[idx];
    e->key = key;
    e->in_use = 1;
    e->s_off = 0;
    index_insert(idx);
    ++g_header->sync_live;
    return e;
}

void release_entry(SyncEntry* e) {
    if (e->s_off != 0) {
        arena_free(kClassVC, e->s_off);
        e->s_off = 0;
    }
    *find_slot(e->key) = kTomb;
    ++g_header->sync_tombstones;
    --g_header->sync_live;
    e->in_use = 0;
    // A free entry's key is the free list's next link.
    e->key = g_header->sync_free_head;
    g_header->sync_free_head = static_cast<uint32_t>(e - g_sync) + 1;
    if (g_header->sync_tombstones > (g_sync_index_mask + 1) / 4) {
        index_rebuild();
    }
}

} // namespace

VectorClock sync_s(uint64_t key) {
    SyncEntry* e = find(key);
    if (e == nullptr || e->s_off == 0) {
        return VectorClock(); // the zero clock: nothing to acquire here
    }
    return VectorClock(static_cast<uint32_t*>(arena_at(e->s_off)));
}

void sync_s_set(uint64_t key, const VectorClock& c) {
    SyncEntry* e = find_or_add(key);
    if (e == nullptr) {
        return;
    }
    if (e->s_off == 0) {
        uint32_t off = arena_alloc(kClassVC);
        if (off == 0) {
            return;
        }
        e->s_off = off;
    }
    VectorClock(static_cast<uint32_t*>(arena_at(e->s_off))).copy_from(c);
}

void sync_s_merge(uint64_t key, const VectorClock& c) {
    SyncEntry* e = find_or_add(key);
    if (e == nullptr) {
        return;
    }
    if (e->s_off == 0) {
        uint32_t off = arena_alloc(kClassVC);
        if (off == 0) {
            return;
        }
        e->s_off = off;
    }
    VectorClock(static_cast<uint32_t*>(arena_at(e->s_off))).merge(c);
}

void sync_s_clear(uint64_t key) {
    SyncEntry* e = find(key);
    if (e != nullptr) {
        release_entry(e);
    }
}

void sync_clear_range(uint64_t begin, uint64_t end) {
    if (atomic_load_u32(&g_header->release_count) == 0) {
        return;
    }
    spin_lock(&g_header->global_lock);
    uint32_t live = g_header->sync_live;
    if (live != 0 && end - begin <= live) {
        // Fewer keys in the range than live entries: look each one up.
        for (uint64_t k = begin; k < end; ++k) {
            SyncEntry* e = find(k);
            if (e != nullptr) {
                release_entry(e);
            }
        }
    } else if (live != 0) {
        uint32_t n = g_header->release_count;
        for (uint32_t i = 0; i < n; ++i) {
            SyncEntry* e = &g_sync[i];
            if (e->in_use && e->key >= begin && e->key < end) {
                release_entry(e);
            }
        }
    }
    spin_unlock(&g_header->global_lock);
}

} // namespace csan
