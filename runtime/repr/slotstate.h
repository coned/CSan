// SPDX-License-Identifier: MIT
// The entries of the publication map P_x and the acquisition map A_x, in one
// encoding.
#ifndef CSAN_SLOTSTATE_H
#define CSAN_SLOTSTATE_H

#include <cstdint>

namespace csan {

//   PubSt ::= None | Pending(E) | Finish(E)
//   AcqSt ::= None | Pending(E) | AllowStore(E_w) | AllowAccess(E_w, E_r)
// The map an entry lives in decides its type. Finish and AllowStore share one
// code; AllowAccess occurs in A_x only. Its E_w is the entry's epoch and its E_r a
// second clock of the same thread (layout.h, SlotMap).
enum SlotState : uint8_t {
    kSlotNone = 0,
    kSlotPending = 1,
    kSlotFinish = 2,
    kSlotAllowStore = 2,
    kSlotAllowAccess = 3,
};

// A set of entry states, one bit each.
constexpr uint8_t slot_bit(SlotState s) {
    return static_cast<uint8_t>(1u << s);
}

} // namespace csan

#endif
