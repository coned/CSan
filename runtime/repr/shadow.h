// SPDX-License-Identifier: MIT
// The per-byte shadow state: the read clock held inside a ByteCell, and the
// split that gives a word's eight bytes cells of their own.
#ifndef CSAN_SHADOW_H
#define CSAN_SHADOW_H

#include "repr/arena.h"
#include "state/layout.h"
#include "repr/slotmap.h"
#include "repr/vectorclock.h"

#include <cstdint>

namespace csan {

// R_x, whichever form the cell holds it in.
//
// rd_le is R_x <= C_tid; rd_set is R_x[tid] <- C_tid[tid], promoting the cell
// from a single packed epoch to a full clock on the second distinct reader;
// rd_reset is R_x <- 0, freeing that clock if one was allocated.
bool rd_le(const ByteCell* c, uint32_t tid);
void rd_set(ByteCell* c, uint32_t tid);
void rd_reset(ByteCell* c);

// The eight per-byte cells of a split word.
ByteCell* split_array(const ByteCell* wc);

// Where the last-write Pos lives for the byte at `addr`: the word's slot while
// the word is unsplit, the byte's own after the split array. Null when
// tracing is off.
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

// The line's last-write Pos for the line holding `addr`; null when tracing is off.
inline uint32_t* line_pos_at(uintptr_t addr) {
    return g_prov ? &g_line_pos[line_cell_at(addr) - g_lines] : nullptr;
}

// The cell of every byte of a line -- the word cell where eight bytes still
// share one, each byte's own cell where the word is split -- as f(cell, addr).
// Caller holds the line's lock.
template <class F>
inline void for_each_byte_cell(uintptr_t line, F f) {
    for (uintptr_t word = line; word < line + kLineBytes; word += kWordBytes) {
        ByteCell* wc = word_cell_at(word);
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

// Give each byte of this word its own cell, every one inheriting the word's
// current state, and return the array. Null if the arena cannot supply the
// blocks, in which case the caller falls back to word granularity -- less
// precise, but not unsound.
ByteCell* ensure_split(ByteCell* wc);

// Collapse a split word back to one cell. Called when a write covers the whole
// word, which makes its eight bytes agree again by definition.
void unsplit(ByteCell* wc);

} // namespace csan

#endif
