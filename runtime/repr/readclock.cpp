// SPDX-License-Identifier: MIT
#include "repr/readclock.h"

#include "repr/arena.h"
#include "repr/vectorclock.h"
#include "state/layout.h"

namespace csan {

static ReadList* block_at(uint32_t off) {
    return static_cast<ReadList*>(arena_at(off));
}

// A fresh block: arena_alloc zeroes it, and zero is not kEpochBot, so the
// entries are stamped empty explicitly.
static uint32_t block_new() {
    uint32_t off = arena_alloc(kClassReadList);
    if (off == 0) {
        return 0;
    }
    ReadList* b = block_at(off);
    for (uint32_t i = 0; i < kReadListEntries; ++i) {
        b->e[i] = kEpochBot;
    }
    b->next = 0;
    return off;
}

uint32_t readlist_new(Epoch first) {
    uint32_t off = block_new();
    if (off != 0) {
        block_at(off)->e[0] = first;
    }
    return off;
}

bool readlist_raise(uint32_t off, uint32_t t, uint32_t c) {
    Epoch* free_slot = nullptr;
    ReadList* last = nullptr;
    for (uint32_t o = off; o != 0; o = last->next) {
        last = block_at(o);
        for (uint32_t i = 0; i < kReadListEntries; ++i) {
            Epoch& e = last->e[i];
            if (e == kEpochBot) {
                if (free_slot == nullptr) {
                    free_slot = &e;
                }
            } else if (epoch_tid(e) == t) {
                if (epoch_clock(e) < c) {
                    e = epoch_make(t, c); // a thread's reads only move forward
                }
                return true;
            }
        }
    }
    if (free_slot != nullptr) {
        *free_slot = epoch_make(t, c);
        return true;
    }
    uint32_t grown = block_new();
    if (grown == 0) {
        return false;
    }
    block_at(grown)->e[0] = epoch_make(t, c);
    last->next = grown;
    return true;
}

bool readlist_le(uint32_t off, uint32_t tid) {
    VectorClock c = thread_vc(tid);
    for (uint32_t o = off; o != 0; o = block_at(o)->next) {
        const ReadList* b = block_at(o);
        for (uint32_t i = 0; i < kReadListEntries; ++i) {
            if (!c.covers(b->e[i])) { // covers(kEpochBot) is true
                return false;
            }
        }
    }
    return true;
}

uint32_t readlist_copy(uint32_t off) {
    uint32_t head = 0;
    uint32_t* link = &head;
    for (uint32_t o = off; o != 0; o = block_at(o)->next) {
        uint32_t copy = arena_alloc(kClassReadList);
        if (copy == 0) {
            readlist_free(head);
            return 0;
        }
        *block_at(copy) = *block_at(o);
        block_at(copy)->next = 0;
        *link = copy;
        link = &block_at(copy)->next;
    }
    return head;
}

void readlist_free(uint32_t off) {
    while (off != 0) {
        uint32_t next = block_at(off)->next;
        arena_free(kClassReadList, off);
        off = next;
    }
}

} // namespace csan
