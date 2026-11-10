// SPDX-License-Identifier: MIT
#include "repr/sync.h"

#include "repr/arena.h"

#include <cstdio>
#include <vector>

namespace csan {

namespace {

uint64_t mix(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return key;
}

SyncStripe* stripe_of(uint64_t key) {
    return &g_sync_stripes[mix(key / kWordBytes) & (kSyncStripes - 1)];
}

SyncEntry* stripe_entries(const SyncStripe* s) {
    return g_sync + static_cast<uint64_t>(s - g_sync_stripes) * g_sync_slots;
}

// `free_slot` gets the first reusable slot on the path. Bounded: sync_join
// probes while a writer may be rearranging the stripe.
SyncEntry* probe(SyncStripe* s, uint64_t key, SyncEntry** free_slot) {
    SyncEntry* e = stripe_entries(s);
    uint32_t mask = g_sync_slots - 1;
    *free_slot = nullptr;
    uint32_t i = static_cast<uint32_t>(mix(key) >> 32) & mask;
    for (uint32_t n = 0; n < g_sync_slots; ++n, i = (i + 1) & mask) {
        if (e[i].state == kSyncLive && e[i].key == key) {
            return &e[i];
        }
        if (e[i].state != kSyncLive && *free_slot == nullptr) {
            *free_slot = &e[i];
        }
        if (e[i].state == kSyncEmpty) {
            return nullptr;
        }
    }
    return nullptr;
}

// Reinsert the live entries so dead slots stop lengthening probes.
void rebuild(SyncStripe* s) {
    SyncEntry* e = stripe_entries(s);
    std::vector<SyncEntry> live;
    for (uint32_t i = 0; i < g_sync_slots; ++i) {
        if (e[i].state == kSyncLive) {
            live.push_back(e[i]);
        }
        e[i].state = kSyncEmpty;
    }
    for (const SyncEntry& x : live) {
        SyncEntry* slot;
        probe(s, x.key, &slot);
        *slot = x;
    }
    s->dead = 0;
}

void drop(SyncStripe* s, SyncEntry* e) {
    if (e == nullptr) {
        return;
    }
    arena_free(kClassVC, e->clock);
    e->state = kSyncDead;
    --s->live;
    ++s->dead;
}

} // namespace

// seq is odd while a writer holds the stripe. Already odd means a writer died
// holding it and the lock was taken over.
SyncRef::SyncRef(uint64_t key) : key_(key), stripe_(stripe_of(key)) {
    spin_lock(&stripe_->lock);
    __atomic_store_n(&stripe_->seq, stripe_->seq | 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    SyncEntry* unused;
    entry_ = probe(stripe_, key, &unused);
}

SyncRef::~SyncRef() {
    __atomic_store_n(&stripe_->seq, stripe_->seq + 1, __ATOMIC_RELEASE);
    spin_unlock(&stripe_->lock);
}

VectorClock SyncRef::clock() const {
    return entry_ != nullptr ? VectorClock(static_cast<uint32_t*>(arena_at(entry_->clock)))
                             : VectorClock();
}

VectorClock SyncRef::clock_or_add() {
    if (entry_ != nullptr) {
        return clock();
    }
    // Keep a quarter of the stripe empty, so every probe ends.
    if (stripe_->live + stripe_->dead >= g_sync_slots / 4 * 3 && stripe_->dead > 0) {
        rebuild(stripe_);
    }
    uint32_t off = 0;
    if (stripe_->live + stripe_->dead >= g_sync_slots / 4 * 3 ||
        (off = arena_alloc(kClassVC)) == 0) {
        if (atomic_fetch_add_u64(&g_header->clock_table_full, 1) == 0) {
            std::fprintf(stderr,
                         "CSAN: sync table full -- happens-before through new locks and atomic "
                         "locations is lost, so FALSE REPORTS are expected from here on. Raise "
                         "CSAN_CLOCK_ENTRIES (or CSAN_ARENA).\n");
            fflush(stderr);
        }
        return VectorClock();
    }
    SyncEntry* slot;
    probe(stripe_, key_, &slot);
    if (slot->state == kSyncDead) {
        --stripe_->dead;
    }
    *slot = SyncEntry{key_, kSyncLive, off};
    ++stripe_->live;
    entry_ = slot;
    return clock();
}

void SyncRef::erase() {
    drop(stripe_, entry_);
    entry_ = nullptr;
}

void SyncRef::erase_byte(uint64_t key) {
    SyncEntry* unused;
    drop(stripe_, probe(stripe_, key, &unused));
}

void sync_join(uint64_t key, VectorClock into) {
    SyncStripe* s = stripe_of(key);
    uint32_t copy[kMaxThreads];
    for (int tries = 0; tries < 64; ++tries) {
        uint32_t seq = __atomic_load_n(&s->seq, __ATOMIC_ACQUIRE);
        if ((seq & 1) != 0) {
            __builtin_ia32_pause();
            continue;
        }
        SyncEntry* unused;
        SyncEntry* e = probe(s, key, &unused);
        uint32_t clock = e != nullptr ? __atomic_load_n(&e->clock, __ATOMIC_RELAXED) : 0;
        uint32_t n = clock != 0 ? live_threads() : 0;
        const uint32_t* row = static_cast<const uint32_t*>(arena_at(clock));
        for (uint32_t i = 0; i < n; ++i) {
            copy[i] = __atomic_load_n(&row[i], __ATOMIC_RELAXED);
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&s->seq, __ATOMIC_RELAXED) == seq) {
            if (n != 0) {
                into.merge(VectorClock(copy));
            }
            return;
        }
    }
    SyncRef r(key); // writers keep changing it, or one died mid-change
    into.merge(r.clock());
}

uint32_t sync_live() {
    uint32_t n = 0;
    for (uint32_t i = 0; i < kSyncStripes; ++i) {
        n += __atomic_load_n(&g_sync_stripes[i].live, __ATOMIC_RELAXED);
    }
    return n;
}

} // namespace csan
