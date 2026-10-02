// SPDX-License-Identifier: MIT
// Relaxed and acquire/release accessors for words in the shared segment.
//
// Plain __atomic builtins on MAP_SHARED memory. They are what makes a field
// safe to touch from several machine processes at once; nothing here implies
// any ordering on the checked program, only on the checker's own metadata.
#ifndef CSAN_ATOMICS_H
#define CSAN_ATOMICS_H

#include <cstdint>

namespace csan {

inline uint32_t atomic_load_u32(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

inline void atomic_store_u32(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

inline uint32_t atomic_fetch_add_u32(uint32_t* p, uint32_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

inline uint64_t atomic_load_u64(const uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_RELAXED);
}

inline uint64_t atomic_fetch_add_u64(uint64_t* p, uint64_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_RELAXED);
}

inline bool atomic_cas_u32(uint32_t* p, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

inline uint32_t atomic_load_u32_acq(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void atomic_store_u32_rel(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

} // namespace csan

#endif
