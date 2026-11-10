// SPDX-License-Identifier: MIT
// Per-location clocks: L_a per lock, S_x per atomic with a live release
// sequence. An open-addressed table split into locked stripes; one word's keys
// share a stripe, held by a SyncRef for its lifetime. lock_key keeps L_a and
// S_x at one address apart.
#ifndef CSAN_SYNC_H
#define CSAN_SYNC_H

#include "repr/vectorclock.h"
#include "state/layout.h"

#include <cstdint>

namespace csan {

inline uint64_t lock_key(uint64_t key) {
    return key | (1ull << 62);
}

class SyncRef {
  public:
    explicit SyncRef(uint64_t key);
    ~SyncRef();
    SyncRef(const SyncRef&) = delete;
    SyncRef& operator=(const SyncRef&) = delete;

    // The clock at this key; the zero clock when there is none.
    VectorClock clock() const;

    // Created zero on first use. The zero clock if the table or arena is full.
    VectorClock clock_or_add();

    void erase();

    // `key`: another byte of this key's word.
    void erase_byte(uint64_t key);

  private:
    uint64_t key_;
    SyncStripe* stripe_;
    SyncEntry* entry_; // null: the key is absent
};

// into ⊔= the clock at `key`. Lock-free seqlock read, so pollers don't
// contend.
void sync_join(uint64_t key, VectorClock into);

// Keys with a clock, over every stripe; read without the stripe locks.
uint32_t sync_live();

} // namespace csan

#endif
