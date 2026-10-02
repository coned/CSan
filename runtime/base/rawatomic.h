// SPDX-License-Identifier: MIT
// The real atomic instructions, plus the flag word the pass uses to describe
// one to the runtime.
//
// These perform the access itself; the rules layer wraps them so the access and
// its clock effect happen inside one critical section. The pass replaces the
// atomic instruction with that call, as ThreadSanitizer's __tsan_atomic_* do:
// hooks placed around the instruction would be separate operations, letting a
// consumer observe a released value before its clock was published.
//
// The access uses genuine atomic builtins, so uninstrumented code touching the
// same location stays correct; only the pairing guarantee is limited to
// instrumented accesses.
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

// Shared with the LLVM pass. Bits 0-1 are the operation's ordering (for
// cmpxchg, its SUCCESS ordering); bit 4 the failure ordering's acquire, which
// only cmpxchg uses. A failing compare-exchange performs no write, so there is
// no failure-release bit.
//
// Bit 5, MFENCED, describes the x86 LOWERING rather than the C++ order -- a
// relaxed fetch_add is still `lock xadd`. It means the lowering is a full
// barrier, so the operation carries MFence's effect on the relays. Set by the
// pass, which is where the lowering is known.
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

// Returns the value that was in memory (equal to `expected` iff the swap
// happened), matching LLVM cmpxchg's first result element.
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
