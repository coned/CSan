// SPDX-License-Identifier: MIT
// Atomic accessors for the checker's own metadata in the shared segment.
// They order only that metadata, never the checked program.
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

// A field some thread reads without the lock its writers hold (a seqlock
// reader): every access is a relaxed atomic, which is a plain mov on x86.
template <class T>
struct Rlx {
    T v;

    Rlx() = default;
    constexpr explicit Rlx(T x) : v(x) {} // a new object: no one else sees it yet
    Rlx(const Rlx& o) : Rlx(o.load()) {}
    T load() const { return __atomic_load_n(&v, __ATOMIC_RELAXED); }
    operator T() const { return load(); }
    Rlx& operator=(T x) {
        __atomic_store_n(&v, x, __ATOMIC_RELAXED);
        return *this;
    }
    Rlx& operator=(const Rlx& o) { return *this = o.load(); }
    // Read-modify-write by the one writer that holds the lock: not an atomic RMW.
    Rlx& operator|=(T x) { return *this = load() | x; }
    Rlx& operator&=(T x) { return *this = load() & x; }
    Rlx& operator++() { return *this = load() + 1; }
    Rlx& operator--() { return *this = load() - 1; }
};

// A flag published once, read on every hook: release store, acquire load.
template <class T>
struct AcqRel {
    T v;

    operator T() const { return __atomic_load_n(&v, __ATOMIC_ACQUIRE); }
    AcqRel& operator=(T x) {
        __atomic_store_n(&v, x, __ATOMIC_RELEASE);
        return *this;
    }
};

} // namespace csan

#endif
