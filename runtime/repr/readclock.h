// SPDX-License-Identifier: MIT
// ReadList: R_x stored as the epochs of its readers since the last write.
// The first reader lives in the ByteCell; more go in these chained blocks.
// Caller holds the line lock.
#ifndef CSAN_READCLOCK_H
#define CSAN_READCLOCK_H

#include "repr/epoch.h"

#include <cstdint>

namespace csan {

// A new one-block list holding `first`. 0 if the arena is exhausted.
uint32_t readlist_new(Epoch first);

// R_x[t] <- c. False if the arena is exhausted; the list is then unchanged,
// which can miss a race but never invent one.
bool readlist_raise(uint32_t off, uint32_t t, uint32_t c);

// Lock-free, on a seqlock snapshot: whether readlist_raise(off, t, c) would
// leave the list unchanged. Offsets are bounds-checked, as a torn read may
// hand over garbage; false when unsure.
bool readlist_holds(uint32_t off, uint32_t t, uint32_t c);

// R_x ⊑ C_tid
bool readlist_le(uint32_t off, uint32_t tid);

// Deep copy, for a word being split. 0 if the arena is exhausted.
uint32_t readlist_copy(uint32_t off);

void readlist_free(uint32_t off);

} // namespace csan

#endif
