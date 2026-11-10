// SPDX-License-Identifier: MIT
// Epoch: a (thread, clock) pair in one word: 32 bits by default, 64 with
// CSAN_EPOCH64 (cmake -DCSAN_EPOCH64=ON) for runs whose clocks pass 24 bits.
#ifndef CSAN_EPOCH_H
#define CSAN_EPOCH_H

#include <cstdint>

namespace csan {

#ifdef CSAN_EPOCH64
typedef uint64_t Epoch;
constexpr uint32_t kTidBits = 16;
#else
typedef uint32_t Epoch;
constexpr uint32_t kTidBits = 8;
#endif
constexpr uint32_t kEpochBits = 8 * sizeof(Epoch);
constexpr uint32_t kClockBits = kEpochBits - kTidBits; // 24, or 48
constexpr Epoch kClockMask = (Epoch{1} << kClockBits) - 1;
constexpr Epoch kEpochBot = 0; // no write / no reader recorded

// The largest clock a thread may reach: the epoch's clock field, or the
// vector clock's 32-bit entry if that is narrower.
constexpr uint32_t kClockLimitBits = kClockBits < 32 ? kClockBits : 32;
constexpr uint32_t kClockLimit =
    kClockLimitBits < 32 ? static_cast<uint32_t>(kClockMask) : UINT32_MAX;

// tid in the top kTidBits, clock below. Clocks start at 1, so zeroed shadow
// is kEpochBot. Clock overflow is reported, never wrapped silently: a
// wrapped clock would make epoch_le() true for an unrelated epoch.

inline uint32_t epoch_tid(Epoch e) {
    return static_cast<uint32_t>(e >> kClockBits);
}

inline uint32_t epoch_clock(Epoch e) {
    return static_cast<uint32_t>(e & kClockMask);
}

inline Epoch epoch_make(uint32_t tid, uint32_t clock) {
    return (static_cast<Epoch>(tid) << kClockBits) | (static_cast<Epoch>(clock) & kClockMask);
}

} // namespace csan

#endif
