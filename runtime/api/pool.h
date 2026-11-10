// SPDX-License-Identifier: MIT
// The CXL pool's region table. The pool is mapped at the same address in
// every process.
#ifndef CSAN_POOL_H
#define CSAN_POOL_H

#include <cstddef>
#include <cstdint>

namespace csan {

// Caller holds the global lock. remove_range_locked returns the removed
// range's end, or 0 if none.
void add_range_locked(uintptr_t base, uintptr_t end);
uintptr_t remove_range_locked(uintptr_t base);

// Mark the lines of [base, base+size) hardware-coherent (`on`) or not.
void mark_hwcc_lines(uintptr_t base, size_t size, bool on);

} // namespace csan

#endif
