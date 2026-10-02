// SPDX-License-Identifier: MIT
// ReadList: R_x as the list of its readers.
//
// The read clock R_x is a vector clock in the model, but it is only raised on
// a read, compared on a write and cleared on install, so it is stored as the
// epochs of the threads that read the byte since its last write: usually one,
// held in the ByteCell itself (shadow.cpp), beyond that in the chained blocks
// handled here (ReadList in layout.h). Caller holds the line's lock.
#ifndef CSAN_READCLOCK_H
#define CSAN_READCLOCK_H

#include "repr/epoch.h"

#include <cstdint>

namespace csan {

// A new one-block list holding `first`. 0 if the arena is exhausted.
uint32_t readlist_new(Epoch first);

// R_x[t] <- c: replace t's entry, or append one, growing the chain if every
// entry is taken. False if a block was needed and the arena could not supply
// it; the list is then unchanged, which can only miss a race, never invent one.
bool readlist_raise(uint32_t off, uint32_t t, uint32_t c);

// R_x <= C_t: every entry is covered by the clock of `tid`.
bool readlist_le(uint32_t off, uint32_t tid);

// A deep copy of the chain, for a word being split. 0 if the arena is exhausted.
uint32_t readlist_copy(uint32_t off);

// Return every block of the chain to the arena.
void readlist_free(uint32_t off);

} // namespace csan

#endif
