// SPDX-License-Identifier: MIT
// The CXL pool: a bump allocator over the segment's pool area, and the table
// of registered regions inside it.
//
// The pool is mapped at the same virtual address in every machine process, so
// a pointer into it is valid everywhere. Freeing does not reclaim -- a bump
// allocator cannot -- it only stops accounting the range.
#ifndef CSAN_POOL_H
#define CSAN_POOL_H

#include <cstddef>
#include <cstdint>

namespace csan {

// The region table. Caller holds the global lock. remove_range_locked returns
// the end of the range it removed, or 0 if `base` named none.
void add_range_locked(uintptr_t base, uintptr_t end);
uintptr_t remove_range_locked(uintptr_t base);

// Mark [base, base+size) as hardware-coherent (`on`) or software-coherent, one
// LineCell flag per line. See the definition for why line granularity is exact.
void mark_hwcc_lines(uintptr_t base, size_t size, bool on);

} // namespace csan

#endif
