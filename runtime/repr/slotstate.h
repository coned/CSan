// SPDX-License-Identifier: MIT
// SlotState: the shared encoding of publication slots (PSlot) and
// acquisition slots (ASlot).
#ifndef CSAN_SLOTSTATE_H
#define CSAN_SLOTSTATE_H

#include <cstdint>

namespace csan {

// Two three-valued types:
//
//   PSlot ::= bot | wb(E)      | published(E)
//   ASlot ::= bot | flushed(E) | acquired(E)
//
// Both are stored with the encoding below. Which type a slot holds is decided
// by the FIELD it lives in, so `flushed` is not representable in a
// publication field:
//
//   in a pub field:  kSlotIntermediate = wb(E)       kSlotFinal = published(E)
//   in an acq field: kSlotIntermediate = flushed(E)  kSlotFinal = acquired(E)
//
// kSlotInvalid is unused by either type; a slot holding it has been corrupted.
enum SlotState : uint8_t {
    kSlotBot = 0,
    kSlotIntermediate = 1,
    kSlotFinal = 2,
    kSlotInvalid = 3,
};

} // namespace csan

#endif
