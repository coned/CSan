// SPDX-License-Identifier: MIT
// The dense clock representation.
//
// VectorClock is `threads` counters in a row, one per thread, addressed by
// thread id. It is what C_t, F^rel_t, F^acq_t, L_a, S_x and V_x use: those are
// indexed by thread and merged whole, so random access is the operation that
// matters.
//
// The read clock R_x wants something else and gets it in readclock.h: it is
// only ever scanned or upserted, never indexed, and almost always holds one
// entry.
#ifndef CSAN_VECTORCLOCK_H
#define CSAN_VECTORCLOCK_H

#include "base/atomics.h"
#include "repr/epoch.h"
#include "state/layout.h"

#include <cstdint>
#include <cstring>

namespace csan {

// A vector clock held as `threads` consecutive 32-bit counters, one per thread
// id, somewhere in the shared segment. The object is a view: it holds a
// pointer to that row and no storage of its own, so copying one is free and
// two views of the same row alias.
//
// A default-constructed VectorClock is the ZERO clock: valid() is false, get()
// returns 0 for every thread, covers() accepts only the bottom epoch, merging
// it into another clock changes nothing, and its own mutators do nothing.
// Callers use that instead of a null check.
//
// covers() and Epoch must agree on kClockBits; both take it from epoch.h.
class VectorClock {
  public:
    VectorClock() : row_(nullptr) {}
    explicit VectorClock(uint32_t* row) : row_(row) {}

    bool valid() const {
        return row_ != nullptr;
    }

    uint32_t get(uint32_t u) const {
        return (row_ != nullptr && u < g_threads) ? atomic_load_u32(&row_[u]) : 0;
    }

    void set(uint32_t u, uint32_t c) {
        if (row_ != nullptr && u < g_threads) {
            atomic_store_u32(&row_[u], c);
        }
    }

    // Write-back, flush and the fences record their epoch, then tick.
    void tick(uint32_t self);

    // ℂ ← ℂ ⊔ other
    void merge(const VectorClock& other) {
        if (row_ == nullptr || other.row_ == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < g_threads; ++i) {
            uint32_t v = atomic_load_u32(&other.row_[i]);
            if (v > atomic_load_u32(&row_[i])) {
                atomic_store_u32(&row_[i], v);
            }
        }
    }

    void copy_from(const VectorClock& other) {
        if (row_ == nullptr || other.row_ == nullptr) {
            return;
        }
        for (uint32_t i = 0; i < g_threads; ++i) {
            atomic_store_u32(&row_[i], atomic_load_u32(&other.row_[i]));
        }
    }

    void clear() {
        if (row_ != nullptr) {
            std::memset(row_, 0, g_threads * sizeof(uint32_t));
        }
    }

    // E ⊑ ℂ
    bool covers(Epoch e) const {
        if (e == kEpochBot) {
            return true;
        }
        uint32_t t = epoch_tid(e);
        return t < g_threads && epoch_clock(e) <= get(t);
    }

  private:
    uint32_t* row_;
};

inline VectorClock thread_vc(uint32_t tid) {
    if (g_vc == nullptr || tid >= g_threads) {
        return VectorClock();
    }
    return VectorClock(&g_vc[static_cast<uint64_t>(tid) * g_threads]);
}

inline uint32_t vc_get(uint32_t tid, uint32_t other) {
    return thread_vc(tid).get(other);
}

// 𝔽^rel_t: the clock captured by t's most recent release fence, or ⊥.
inline VectorClock frel_vc(uint32_t tid) {
    if (g_frel == nullptr || tid >= g_threads) {
        return VectorClock();
    }
    return VectorClock(&g_frel[static_cast<uint64_t>(tid) * g_threads]);
}

inline bool frel_is_set(uint32_t tid) {
    return g_frel_set != nullptr && tid < g_threads && g_frel_set[tid] != 0;
}

inline void frel_mark(uint32_t tid) {
    if (g_frel_set != nullptr && tid < g_threads) {
        g_frel_set[tid] = 1;
    }
}

// 𝔽^acq_t: clocks t has observed with a non-acquiring atomic read but not yet
// joined. An acquire fence joins this into ℂ_t.
inline VectorClock facq_vc(uint32_t tid) {
    if (g_facq == nullptr || tid >= g_threads) {
        return VectorClock();
    }
    return VectorClock(&g_facq[static_cast<uint64_t>(tid) * g_threads]);
}

// now(t) = ℂ_t[t]@t
inline Epoch now_epoch(uint32_t tid) {
    return epoch_make(tid, vc_get(tid, tid));
}

// ℂ_t[t] ← ℂ_t[t] + 1: the clock increment every rule ends with when it
// released something or recorded an epoch.
inline void tick(uint32_t tid) {
    thread_vc(tid).tick(tid);
}

// E ⊑ ℂ_t
inline bool epoch_le(Epoch e, uint32_t tid) {
    return thread_vc(tid).covers(e);
}

} // namespace csan

#endif
