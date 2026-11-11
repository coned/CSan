// SPDX-License-Identifier: MIT
// Walks over the shadow of a byte range: its lines, their words, and the
// split-or-not choice for each word. Storage only: what to check at each
// cell is the caller's (rules/). All templates, so they inline into the rule.
#ifndef CSAN_WALK_H
#define CSAN_WALK_H

#include "repr/shadow.h"
#include "state/layout.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace csan {

// The part [lo, hi) of one line `line` that an access covers.
struct LineSpan {
    Shadow s;
    LineCell* lc;
    uintptr_t line;
    uintptr_t lo;
    uintptr_t hi;
};

// on_line(span) for each line [begin, begin+n) touches that is checked, with
// its lock held; on_hwcc(span) for each hardware-coherent one, unlocked.
// `s`: the region's shadow, resolved by the caller.
template <class F, class G>
inline void for_each_line_span(Shadow s, uintptr_t begin, size_t n, F on_line, G on_hwcc) {
    uintptr_t end = begin + static_cast<uintptr_t>(n);
    for (uintptr_t a = begin; a < end;) {
        uintptr_t line = a & ~(kLineBytes - 1);
        LineSpan sp{s, s.line(line), line, a, std::min(end, line + kLineBytes)};
        if (line_is_hwcc(sp.lc)) {
            on_hwcc(sp);
        } else {
            line_lock(sp.lc);
            on_line(sp);
            line_unlock(sp.lc);
        }
        a = sp.hi;
    }
}

template <class F, class G>
inline void for_each_line_span(uintptr_t begin, size_t n, F on_line, G on_hwcc) {
    for_each_line_span(shadow_of(begin), begin, n, on_line, on_hwcc);
}

// f(wc, word, lo, hi) for each word the span covers, [lo, hi) its covered part.
template <class F>
inline void for_each_word_in(const LineSpan& sp, F f) {
    for (uintptr_t a = sp.lo; a < sp.hi;) {
        uintptr_t word = a & ~(kWordBytes - 1);
        uintptr_t hi = std::min(sp.hi, word + kWordBytes);
        f(sp.s.cell(word), word, a, hi);
        a = hi;
    }
}

// f(cell, addr) for each cell an access to the span reads: a whole unsplit
// word is one cell; a partly covered word is split first, so its untouched
// bytes keep their state. If the split fails the word cell stands in (less
// precise, not unsound).
template <class F>
inline void for_each_cell_read(const LineSpan& sp, F f) {
    for_each_word_in(sp, [&](ByteCell* wc, uintptr_t word, uintptr_t lo, uintptr_t hi) {
        bool whole = lo == word && hi == word + kWordBytes;
        ByteCell* bytes = whole && !cell_split(wc) ? nullptr : ensure_split(wc);
        if (bytes == nullptr) {
            f(wc, word);
            return;
        }
        for (uintptr_t b = lo; b < hi; ++b) {
            f(&bytes[b & (kWordBytes - 1)], b);
        }
    });
}

// As for_each_cell_read, for a write. A whole word that is split is visited
// byte by byte (only each byte's cell holds its state), then collapsed back
// to one cell if its bytes now agree; on_collapse(wc, word) runs on it after.
template <class F, class C>
inline void for_each_cell_written(const LineSpan& sp, F f, C on_collapse) {
    for_each_word_in(sp, [&](ByteCell* wc, uintptr_t word, uintptr_t lo, uintptr_t hi) {
        bool whole = lo == word && hi == word + kWordBytes;
        if (whole && cell_split(wc)) {
            ByteCell* bytes = split_array(wc);
            for (uint32_t i = 0; i < kWordBytes; ++i) {
                f(&bytes[i], word + i);
            }
            if (collapse_if_uniform(wc)) {
                on_collapse(wc, word);
            }
            return;
        }
        ByteCell* bytes = whole ? nullptr : ensure_split(wc);
        if (bytes == nullptr) {
            f(wc, word);
            return;
        }
        for (uintptr_t b = lo; b < hi; ++b) {
            f(&bytes[b & (kWordBytes - 1)], b);
        }
    });
}

// The first byte y ∈ c ∖ X, X = [lo, hi) the span's bytes, whose cell
// satisfies pred, as {address, cell}; {0, nullptr} if none. An unsplit word
// answers for all its bytes.
struct OutsideCell {
    uintptr_t addr;
    const ByteCell* cell;
};

template <class P>
inline OutsideCell find_cell_outside(const LineSpan& sp, P pred) {
    for (uintptr_t word = sp.line; word < sp.line + kLineBytes; word += kWordBytes) {
        if (word >= sp.lo && word + kWordBytes <= sp.hi) {
            continue; // overwritten whole
        }
        ByteCell* wc = sp.s.cell(word);
        if (!cell_split(wc)) {
            if (pred(wc)) {
                // a byte of this word outside X
                return {word >= sp.lo && word < sp.hi ? sp.hi : word, wc};
            }
            continue;
        }
        ByteCell* bytes = split_array(wc);
        for (uintptr_t b = word; b < word + kWordBytes; ++b) {
            if ((b < sp.lo || b >= sp.hi) && pred(&bytes[b - word])) {
                return {b, &bytes[b - word]};
            }
        }
    }
    return {0, nullptr};
}

// f(cell) for each cell holding the byte at `addr`'s word: the word cell, or
// all eight byte cells if it is split. Caller holds the line lock.
template <class F>
inline void for_each_cell_of_word(uintptr_t addr, F f) {
    ByteCell* wc = word_cell_at(addr);
    if (!cell_split(wc)) {
        f(wc);
        return;
    }
    ByteCell* bytes = split_array(wc);
    for (uint32_t i = 0; i < kWordBytes; ++i) {
        f(&bytes[i]);
    }
}

} // namespace csan

#endif
