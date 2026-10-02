// SPDX-License-Identifier: MIT
// VectorClock::tick, the only clock operation that can fail.
#include "repr/vectorclock.h"

#include <cstdio>

namespace csan {

void VectorClock::tick(uint32_t self) {
    if (row_ == nullptr || self >= g_threads) {
        return;
    }
    uint32_t next = atomic_fetch_add_u32(&row_[self], 1) + 1;
    if (next > kClockMask) {
        // Wrapping would make epoch_le() answer true for an unrelated epoch:
        // a silently missed race. Say so once and keep counting.
        if (atomic_fetch_add_u64(&g_header->epoch_overflow, 1) == 0) {
            std::fprintf(stderr,
                         "CSAN: thread %u clock exceeded %u bits -- epochs can no "
                         "longer be compared correctly and races WILL be missed from here "
                         "on. Rebuild with a wider kClockBits.\n",
                         self, kClockBits);
            fflush(stderr);
        }
    }
}


} // namespace csan
