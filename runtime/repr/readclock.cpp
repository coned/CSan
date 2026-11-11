// SPDX-License-Identifier: MIT
#include "repr/readclock.h"

#include "repr/arena.h"
#include "repr/vectorclock.h"
#include "state/layout.h"

namespace csan {

static ReadList* block_at(uint32_t off) {
    return static_cast<ReadList*>(arena_at(off));
}

// Zeroed block: every entry starts as kEpochBot.
uint32_t readlist_new(Epoch first) {
    uint32_t off = arena_alloc(kClassReadList);
    if (off != 0) {
        block_at(off)->e[0] = first;
    }
    return off;
}

bool readlist_raise(uint32_t off, uint32_t t, uint32_t c) {
    Rlx<Epoch>* free_slot = nullptr;
    ReadList* last = nullptr;
    for (uint32_t o = off; o != 0; o = last->next) {
        last = block_at(o);
        for (uint32_t i = 0; i < kReadListEntries; ++i) {
            Rlx<Epoch>& e = last->e[i];
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
    uint32_t grown = arena_alloc(kClassReadList);
    if (grown == 0) {
        return false;
    }
    block_at(grown)->e[0] = epoch_make(t, c);
    last->next = grown;
    return true;
}

bool readlist_holds(uint32_t off, uint32_t t, uint32_t c) {
    uint64_t limit = g_header->arena_size;
    for (uint32_t o = off, n = 0; o != 0; ++n) {
        if (n == 64 || o % 8 != 0 || o + sizeof(ReadList) > limit) {
            return false;
        }
        const ReadList* b = block_at(o);
        for (uint32_t i = 0; i < kReadListEntries; ++i) {
            Epoch e = b->e[i];
            if (e != kEpochBot && epoch_tid(e) == t) {
                return epoch_clock(e) >= c;
            }
        }
        o = b->next;
    }
    return false;
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
    ReadList* prev = nullptr;
    for (uint32_t o = off; o != 0; o = block_at(o)->next) {
        uint32_t copy = arena_alloc(kClassReadList);
        if (copy == 0) {
            readlist_free(head);
            return 0;
        }
        *block_at(copy) = *block_at(o);
        block_at(copy)->next = 0;
        if (prev != nullptr) {
            prev->next = copy;
        } else {
            head = copy;
        }
        prev = block_at(copy);
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
