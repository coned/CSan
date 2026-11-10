// SPDX-License-Identifier: MIT
// Per-byte shadow storage: the read record in a ByteCell, and word splitting.
// No rule decisions here; those are in rules/.
#ifndef CSAN_SHADOW_H
#define CSAN_SHADOW_H

#include "repr/arena.h"
#include "repr/readclock.h"
#include "repr/slotmap.h"
#include "repr/vectorclock.h"
#include "state/layout.h"

#include <cstdint>

namespace csan {

// R_x storage only; the rules over it (R_x ⊑ C_t, addRead) are in
// rules/rules_access.cpp. `rd` holds one Epoch, or, with kRdSharedBit set,
// the offset of a ReadList (readclock.h).

// Empty the record, freeing a ReadList if there is one.
inline void rd_clear(ByteCell* c) {
    if (cell_rd_shared(c)) {
        readlist_free(cell_rd_off(c));
        c->pub &= ~kRdSharedBit;
    }
    c->rd = kEpochBot;
}

// Turn the one-Epoch record into a ReadList holding that Epoch and
// epoch_make(tid, now). Left as it was if the arena is exhausted, which can
// only miss a race.
void rd_to_list(ByteCell* c, uint32_t tid, uint32_t now);

// The eight per-byte cells of a split word.
ByteCell* split_array(const ByteCell* wc);

// Last-write Pos for the byte at `addr`: the word's while unsplit, else the
// byte's own. Null when tracing is off.
inline uint32_t* write_pos_at(uintptr_t addr) {
    if (!g_prov) {
        return nullptr;
    }
    ByteCell* wc = word_cell_at(addr);
    if (!cell_split(wc)) {
        return &g_cell_pos[wc - g_cells];
    }
    return reinterpret_cast<uint32_t*>(split_array(wc) + kWordBytes) + (addr & (kWordBytes - 1));
}

// Record `pos` as the last write's Pos for the byte at `addr` (no-op when off).
inline void set_write_pos(uintptr_t addr, uint32_t pos) {
    if (uint32_t* p = write_pos_at(addr)) {
        *p = pos;
    }
}

// f(cell, addr) for each cell of a line (word cell, or byte cells if split).
// Caller holds the line lock.
template <class F>
inline void for_each_byte_cell(const Shadow& s, uintptr_t line, F f) {
    for (uintptr_t word = line; word < line + kLineBytes; word += kWordBytes) {
        ByteCell* wc = s.cell(word);
        if (!cell_split(wc)) {
            f(wc, word);
            continue;
        }
        ByteCell* bytes = split_array(wc);
        for (uint32_t i = 0; i < kWordBytes; ++i) {
            f(&bytes[i], word + i);
        }
    }
}

// Free everything a cell points into and leave it empty.
void cell_reset_state(ByteCell* c);

// Give each byte its own cell, inheriting the word's state. Null if the arena
// is exhausted; the caller stays at word granularity (less precise, not
// unsound).
ByteCell* ensure_split(ByteCell* wc);

// Collapse a split word, dropping its bytes' state.
void unsplit(ByteCell* wc);

// After a store to the whole of a split word: collapse it, keeping the state,
// if its eight bytes hold the same state. True if it did.
bool collapse_if_uniform(ByteCell* wc);

// Before S_x is set at `key`: set kSyncBit so a plain write there ends the
// sequence. Takes the line lock, so the caller must hold no sync stripe.
void mark_release_site(uint64_t key);

// Plain write to [lo, hi) in `wc`'s word: S_x <- 0 at each flagged byte.
// Caller holds the line lock.
void end_flagged_sequences(ByteCell* wc, uint64_t lo, uint64_t hi);

inline void end_release_sequences(ByteCell* wc, uint64_t lo, uint64_t hi) {
    if (cell_split(wc) || (wc->pub & kSyncBit) != 0) {
        end_flagged_sequences(wc, lo, hi);
    }
}

} // namespace csan

#endif
