// SPDX-License-Identifier: MIT
#include "repr/vectorclock.h"

#include <cstdio>

namespace csan {

void clock_overflow(uint32_t self) {
    if (atomic_fetch_add_u64(&g_header->epoch_overflow, 1) == 0) {
        std::fprintf(stderr,
                     "CSAN: thread %u clock exceeded %u bits -- races WILL be missed from here "
                     "on. Rebuild with -DCSAN_EPOCH64=ON.\n",
                     self, kClockLimitBits);
        fflush(stderr);
    }
}

} // namespace csan
