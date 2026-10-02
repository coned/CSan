// SPDX-License-Identifier: MIT
// Epoch: a (thread, clock) pair in one 32-bit word.
#ifndef CSAN_EPOCH_H
#define CSAN_EPOCH_H

#include <cstdint>

namespace csan {

constexpr uint32_t kTidBits = 8;
constexpr uint32_t kClockBits = 32 - kTidBits; // 24
constexpr uint32_t kClockMask = (1u << kClockBits) - 1;
constexpr uint32_t kEpochBot = UINT32_MAX; // no write / no reader recorded

//
//   bit  31            24 23                             0
//       +----------------+-------------------------------+
//       |   tid (8)      |          clock (24)           |
//       +----------------+-------------------------------+
//
//   tid    which thread the epoch belongs to. 8 bits covers kMaxThreads=256.
//   clock  that thread's own clock component at the moment recorded (24 bits).
//
//   kEpochBot (all ones) is the empty value: no write, or no reader, recorded.
//   It is not a valid packing -- tid 255 with clock 0xFFFFFF -- so it can never
//   collide with a real epoch.
//
// Exceeding the clock field is reported, never wrapped: a wrapped clock makes
// epoch_le() answer true for an unrelated epoch.
typedef uint32_t Epoch;

inline uint32_t epoch_tid(Epoch e) {
    return e >> kClockBits;
}

inline uint32_t epoch_clock(Epoch e) {
    return e & kClockMask;
}

inline Epoch epoch_make(uint32_t tid, uint32_t clock) {
    return (tid << kClockBits) | (clock & kClockMask);
}

} // namespace csan

#endif
