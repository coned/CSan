// SPDX-License-Identifier: MIT
// The real atomic accesses, and the flag word the pass passes to the runtime.
// The pass replaces the instruction with a call (like __tsan_atomic_*) so the
// access and its clock effect share one critical section; separate hooks would
// let a consumer see a released value before its clock was published.
#ifndef CSAN_RAWATOMIC_H
#define CSAN_RAWATOMIC_H

#include <cstdint>

namespace csan {

enum AtomicRmwOp {
    kRmwXchg = 0,
    kRmwAdd = 1,
    kRmwSub = 2,
    kRmwAnd = 3,
    kRmwOr = 4,
    kRmwXor = 5,
};

// Shared with the LLVM pass. Bits 0-1: ordering (cmpxchg: success ordering).
// Bit 4: cmpxchg failure acquire (a failed CAS writes nothing, so no release).
// Bit 5: the x86 lowering is a full barrier (e.g. relaxed `lock xadd`), so the
// op also has MFence's effect on the relays.
enum AtomicFlags {
    kAtomicAcquire = 1u << 0,
    kAtomicRelease = 1u << 1,
    kAtomicFailAcquire = 1u << 4,
    kAtomicMFenced = 1u << 5,
};

inline uint64_t raw_atomic_load(const void* p, uint32_t size) {
    switch (size) {
    case 1:
        return __atomic_load_n(static_cast<const uint8_t*>(p), __ATOMIC_SEQ_CST);
    case 2:
        return __atomic_load_n(static_cast<const uint16_t*>(p), __ATOMIC_SEQ_CST);
    case 4:
        return __atomic_load_n(static_cast<const uint32_t*>(p), __ATOMIC_SEQ_CST);
    default:
        return __atomic_load_n(static_cast<const uint64_t*>(p), __ATOMIC_SEQ_CST);
    }
}

inline void raw_atomic_store(void* p, uint64_t v, uint32_t size) {
    switch (size) {
    case 1:
        __atomic_store_n(static_cast<uint8_t*>(p), static_cast<uint8_t>(v), __ATOMIC_SEQ_CST);
        break;
    case 2:
        __atomic_store_n(static_cast<uint16_t*>(p), static_cast<uint16_t>(v), __ATOMIC_SEQ_CST);
        break;
    case 4:
        __atomic_store_n(static_cast<uint32_t*>(p), static_cast<uint32_t>(v), __ATOMIC_SEQ_CST);
        break;
    default:
        __atomic_store_n(static_cast<uint64_t*>(p), v, __ATOMIC_SEQ_CST);
        break;
    }
}

#define CXL_RMW_FOR_TYPE(T)                                                                        \
    do {                                                                                           \
        T* q = static_cast<T*>(p);                                                                 \
        T x = static_cast<T>(v);                                                                   \
        switch (op) {                                                                              \
        case kRmwXchg:                                                                             \
            return __atomic_exchange_n(q, x, __ATOMIC_SEQ_CST);                                    \
        case kRmwAdd:                                                                              \
            return __atomic_fetch_add(q, x, __ATOMIC_SEQ_CST);                                     \
        case kRmwSub:                                                                              \
            return __atomic_fetch_sub(q, x, __ATOMIC_SEQ_CST);                                     \
        case kRmwAnd:                                                                              \
            return __atomic_fetch_and(q, x, __ATOMIC_SEQ_CST);                                     \
        case kRmwOr:                                                                               \
            return __atomic_fetch_or(q, x, __ATOMIC_SEQ_CST);                                      \
        default:                                                                                   \
            return __atomic_fetch_xor(q, x, __ATOMIC_SEQ_CST);                                     \
        }                                                                                          \
    } while (0)

inline uint64_t raw_atomic_rmw(void* p, uint64_t v, uint32_t size, uint32_t op) {
    switch (size) {
    case 1:
        CXL_RMW_FOR_TYPE(uint8_t);
    case 2:
        CXL_RMW_FOR_TYPE(uint16_t);
    case 4:
        CXL_RMW_FOR_TYPE(uint32_t);
    default:
        CXL_RMW_FOR_TYPE(uint64_t);
    }
}
#undef CXL_RMW_FOR_TYPE

// Returns the old value (== expected iff swapped), like LLVM cmpxchg.
inline uint64_t raw_atomic_cas(void* p, uint64_t expected, uint64_t desired, uint32_t size) {
    switch (size) {
    case 1: {
        uint8_t e = static_cast<uint8_t>(expected);
        __atomic_compare_exchange_n(static_cast<uint8_t*>(p), &e, static_cast<uint8_t>(desired),
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    case 2: {
        uint16_t e = static_cast<uint16_t>(expected);
        __atomic_compare_exchange_n(static_cast<uint16_t*>(p), &e, static_cast<uint16_t>(desired),
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    case 4: {
        uint32_t e = static_cast<uint32_t>(expected);
        __atomic_compare_exchange_n(static_cast<uint32_t*>(p), &e, static_cast<uint32_t>(desired),
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    default: {
        uint64_t e = expected;
        __atomic_compare_exchange_n(static_cast<uint64_t*>(p), &e, desired, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    }
}

} // namespace csan

#endif
