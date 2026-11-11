// SPDX-License-Identifier: MIT
// VectorClock: a row of the segment; C_t, F^rel_t, F^acq_t, L_a, S_x.
// Tids are never reused, so entries past live_threads() are zero.
// One writer per row (the owning thread, or the sync stripe holder); entries
// are relaxed atomics, since sync_join reads L_a and S_x rows unlocked.
#ifndef CSAN_VECTORCLOCK_H
#define CSAN_VECTORCLOCK_H

#include "repr/epoch.h"
#include "state/layout.h"

#include <algorithm>
#include <cstdint>

namespace csan {

// Reports, once, a clock grown past kClockLimitBits.
[[gnu::cold]] void clock_overflow(uint32_t self);

inline uint32_t live_threads() {
    return __atomic_load_n(&g_header->next_tid, __ATOMIC_RELAXED);
}

// A view of a row. Default-constructed is the zero clock; writes to it are
// dropped, so callers need no null check.
class VectorClock {
  public:
    VectorClock() = default;
    explicit VectorClock(uint32_t* row) : row_(row) {}

    bool valid() const {
        return row_ != nullptr;
    }

    uint32_t get(uint32_t u) const {
        return row_ != nullptr ? ld(u) : 0;
    }

    void set(uint32_t u, uint32_t c) {
        if (row_ != nullptr) {
            st(u, c);
        }
    }

    // C[self] += 1
    void tick(uint32_t self) {
        if (row_ == nullptr) {
            return;
        }
        uint32_t c = ld(self);
        st(self, c + 1);
        if (c >= kClockLimit) {
            clock_overflow(self);
        }
    }

    // C <- C ⊔ other
    void merge(const VectorClock& other) {
        if (row_ == nullptr || other.row_ == nullptr) {
            return;
        }
        for (uint32_t i = 0, n = live_threads(); i < n; ++i) {
            uint32_t a = ld(i), b = other.ld(i);
            if (b > a) {
                st(i, b);
            }
        }
    }

    // C <- other
    void copy_from(const VectorClock& other) {
        if (row_ == nullptr || other.row_ == nullptr) {
            return;
        }
        for (uint32_t i = 0, n = live_threads(); i < n; ++i) {
            st(i, other.ld(i));
        }
    }

    // E ⊑ C
    bool covers(Epoch e) const {
        return epoch_clock(e) <= get(epoch_tid(e));
    }

  private:
    uint32_t ld(uint32_t u) const { return __atomic_load_n(&row_[u], __ATOMIC_RELAXED); }
    void st(uint32_t u, uint32_t c) { __atomic_store_n(&row_[u], c, __ATOMIC_RELAXED); }

    uint32_t* row_ = nullptr;
};

inline VectorClock thread_vc(uint32_t tid) {
    return VectorClock(&g_vc[static_cast<uint64_t>(tid) * g_threads]);
}

// F^rel_t: the clock captured by t's last release fence, or ⊥.
inline VectorClock frel_vc(uint32_t tid) {
    return VectorClock(&g_frel[static_cast<uint64_t>(tid) * g_threads]);
}

inline bool frel_is_set(uint32_t tid) {
    return g_frel_set[tid] != 0;
}

inline void frel_mark(uint32_t tid) {
    g_frel_set[tid] = 1;
}

// F^acq_t: what t's relaxed loads observed, for its next acquire fence.
inline VectorClock facq_vc(uint32_t tid) {
    return VectorClock(&g_facq[static_cast<uint64_t>(tid) * g_threads]);
}

inline uint32_t vc_get(uint32_t tid, uint32_t other) {
    return thread_vc(tid).get(other);
}

// now(t) = C_t[t]@t
inline Epoch now_epoch(uint32_t tid) {
    return epoch_make(tid, vc_get(tid, tid));
}

inline void tick(uint32_t tid) {
    thread_vc(tid).tick(tid);
}

// E ⊑ C_t
inline bool epoch_le(Epoch e, uint32_t tid) {
    return thread_vc(tid).covers(e);
}

} // namespace csan

#endif
