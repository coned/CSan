// SPDX-License-Identifier: MIT
// Read and Write for plain accesses, the visibility predicates and Install
// they use, and the walk that applies them to every byte of a range.
#include "rules/rules.h"

#include "base/atomics.h"
#include "base/report.h"
#include "base/trace.h"
#include "repr/shadow.h"
#include "repr/slotmap.h"
#include "repr/sync.h"

#include <algorithm>

namespace csan {

// ---- visibility predicates ---------------------------------------------------

// ReadVis(x,t) ⟺ mach(thr(W^B_x)) = mach(t) ∨ Acq^B(x, mach(t), ℂ_t)
//
// A byte nobody has written is trivially visible. Acq^B is consulted only when
// the writer was on another machine, which is why 𝔸^B_x is read nowhere else.
bool read_vis(const ByteCell* x, uint32_t t) {
    if (x->wb == kEpochBot) {
        return true;
    }
    return machine_of(epoch_tid(x->wb)) == machine_of(t) || acq_ok(cell_acq_off(x), t);
}

// WriteVis(ℓ,t) ⟺ mach(thr(W^L_ℓ)) = mach(t) ∨ Acq^L(ℓ, mach(t), ℂ_t)
bool write_vis(const LineCell* l, uint32_t t) {
    if (l->wl == kEpochBot) {
        return true;
    }
    return machine_of(epoch_tid(l->wl)) == machine_of(t) || acq_ok(l->acq, t);
}

// WriteVisFull(ℓ,t) ⟺ mach(thr(W^L_ℓ)) = mach(t) ∨ Pub^L(ℓ, ℂ_t) ∨ Acq^L(ℓ, mach(t), ℂ_t)
//
// The premise for a store covering the whole line: no stale copy can ride
// back out, so no invalidate is needed, but the previous writer's line must
// still be published or its pending write-back would overwrite this store.
// Acq^L stays as an alternative so this is a pure weakening of WriteVis.
bool write_vis_full_line(const LineCell* l, uint32_t t) {
    if (l->wl == kEpochBot) {
        return true;
    }
    if (machine_of(epoch_tid(l->wl)) == machine_of(t)) {
        return true;
    }
    return pub_ok(l->pub, t) || acq_ok(l->acq, t);
}

// Install_{x,E}: W^B_x, W^L_ℓ ← E; ℝ_x ← 0; ℙ^B_x, 𝔸^B_x, ℙ^L_ℓ, 𝔸^L_ℓ ← ∅.
// It does not reset the byte relays of other bytes on the same line.
void install(ByteCell* x, LineCell* l, Epoch E) {
    rd_reset(x);
    uint32_t p = cell_pub_off(x);
    slotmap_release(&p);
    cell_set_pub_off(x, p);
    uint32_t q = cell_acq_off(x);
    slotmap_release(&q);
    cell_set_acq_off(x, q);
    x->wb = E;
    l->wl = E;
    slotmap_release(&l->pub);
    slotmap_release(&l->acq);
}

// ---- the rules, per byte cell -----------------------------------------------
//
// One cell stands for one byte, or for a whole word while its eight bytes still
// share a cell (see ByteCell). The caller holds the line's lock.

// Read(x,t):       W^B_x ⊑ ℂ_t  ∧  ReadVis(x,t)    ⟹  ℝ_x[t] ← ℂ_t[t];  atomic_x ← ⊥
// LoadRlx/LoadAcq: atomic_x = ⊤ ∨ W^B_x ⊑ ℂ_t                ⟹  ..., atomic_x ← ⊤
//
// Atomicity discharges the ordering premise when the byte's previous access was
// atomic too. An atomic access is also exempt from the visibility premise: the
// C++ standard requires no timeliness. A plain access is never exempt.
static void read_byte(ByteCell* x, uintptr_t addr, uint32_t t, bool atomic) {
    bool both_atomic = atomic && cell_atomic(x);
    if (!both_atomic && !epoch_le(x->wb, t)) {
        report_race(CSAN_RACE_READ_BEFORE_HB_WRITE, addr, t, x->wb);
    }
    if (!atomic && !read_vis(x, t)) {
        report_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE, addr, t, x->wb);
    }
    if (trace_hit(addr)) {
        trace_event("READ", addr, t, "");
    }
    rd_set(x, t);
    cell_set_atomic(x, atomic);
}

// Write(x,t):  W^B_x ⊑ ℂ_t  ∧  LineOrd(ℓ,x,t)  ∧  ℝ_x ⊑ ℂ_t  ∧  WriteVis(ℓ,t)  ⟹  Install_{x,E};  atomic_x ← ⊥
//   LineOrd(ℓ,x,t) ⟺ W^L_ℓ = W^B_x ∨ mach(thr(W^L_ℓ)) = mach(t) ∨ W^L_ℓ ⊑ ℂ_t
// The store rules: atomic_x = ⊤ ∨ (W^B_x ⊑ ℂ_t ∧ LineOrd(ℓ,x,t) ∧ ℝ_x ⊑ ℂ_t)  ⟹  ..., atomic_x ← ⊤
// An atomic store is exempt from the per-byte WriteVis check: the line-level
// hazard it can still cause is checked once per line, in
// check_atomic_line_write, which is where two atomic stores to different
// locations on one line are caught.
static void write_byte(ByteCell* x, LineCell* l, uintptr_t addr, uint32_t t, Epoch E,
                       bool atomic, bool covers_line) {
    bool both_atomic = atomic && cell_atomic(x);
    if (!both_atomic && !epoch_le(x->wb, t)) {
        report_race(CSAN_RACE_DATA_RACE, addr, t, x->wb);
    }
    // Only another machine's write-back of a different byte can lose this one.
    if (!both_atomic && l->wl != x->wb && !epoch_le(l->wl, t) &&
        machine_of(epoch_tid(l->wl)) != machine_of(t)) {
        report_race(CSAN_RACE_WRITE_BEFORE_HB_LINE_WRITE, addr, t, l->wl);
    }
    if (!both_atomic && !rd_le(x, t)) {
        report_race(CSAN_RACE_WRITE_AFTER_READ, addr, t);
    }
    // Only this premise weakens for a whole-line store; the two above are about
    // ordering, which covering the line does nothing for.
    if (!atomic && !(covers_line ? write_vis_full_line(l, t) : write_vis(l, t))) {
        report_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE, addr, t, l->wl);
    }
    if (trace_hit(addr)) {
        trace_event("WRITE", addr, t, "");
    }
    install(x, l, E);
    if (uint32_t* pos = write_pos_at(addr)) {
        *pos = trace_cur_pos();
        *line_pos_at(addr) = trace_cur_pos();
    }
    cell_set_atomic(x, atomic);
}

// Two ATOMIC writes to one line from two machines, at DIFFERENT locations.
// Each writes the whole line back carrying its stale copy of the other's bytes,
// so one value is lost: the unit moving between a cache and CXL memory is the
// line, not the location. Two atomic writes to the SAME location are not this
// and are not reported.
//
// Cleared by the chain a plain write needs -- write, clwb, sfence, then
// clflushopt, sfence on the other machine -- which is WriteVis. The clwb and
// sfence the runtime injects around an atomic store (rules_sync.cpp) are real
// instructions, not events, and do not count.
static void check_atomic_line_write(LineCell* l, uintptr_t addr, uint32_t t, uint32_t off,
                                    uint32_t len) {
    if (l->wl == kEpochBot || l->wl_len == 0) {
        return; // nothing has written this line yet
    }
    if (l->wl_off == off && l->wl_len == len) {
        return; // the same location: no line hazard
    }
    if (write_vis(l, t)) {
        return; // this machine owns the line, or it acquired it
    }
    report_race(CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE, addr, t, l->wl);
}

// ---- the walk over a byte range ---------------------------------------------

// Visit the words of [begin, begin+n) one line at a time, holding that line's
// lock: f(lc, word, lo, hi) for each word, where [lo, hi) is the part of the
// word the range covers. Hardware-coherent lines are skipped whole.
template <class F>
static void for_each_covered_word(uintptr_t begin, size_t n, F f) {
    uintptr_t end = begin + static_cast<uintptr_t>(n);
    uintptr_t a = begin;
    while (a < end) {
        uintptr_t line = a & ~(kLineBytes - 1);
        uintptr_t line_end = std::min(end, line + kLineBytes);
        LineCell* lc = line_cell_at(line);
        if (line_is_hwcc(lc)) {
            a = line_end;
            continue;
        }
        line_lock(lc);
        while (a < line_end) {
            uintptr_t word = a & ~(kWordBytes - 1);
            uintptr_t hi = std::min(line_end, word + kWordBytes);
            f(lc, word, a, hi);
            a = hi;
        }
        line_unlock(lc);
    }
}

// A word whose bytes are all covered and not already split is one cell; any
// other word is split so the bytes the access does not touch keep their own
// state. If the arena cannot supply the split, the word cell stands in for
// its bytes: word granularity, less precise but not unsound.
void read_range(uintptr_t begin, size_t n, uint32_t t, bool atomic) {
    for_each_covered_word(begin, n, [&](LineCell*, uintptr_t word, uintptr_t lo, uintptr_t hi) {
        ByteCell* wc = word_cell_at(word);
        bool whole = lo == word && hi == word + kWordBytes;
        if (whole && !cell_split(wc)) {
            read_byte(wc, word, t, atomic);
            return;
        }
        ByteCell* bytes = ensure_split(wc);
        if (bytes == nullptr) {
            read_byte(wc, word, t, atomic);
            return;
        }
        for (uintptr_t b = lo; b < hi; ++b) {
            read_byte(&bytes[b & (kWordBytes - 1)], b, t, atomic);
        }
    });
    atomic_fetch_add_u64(&g_header->read_events, n);
}

void write_range(uintptr_t begin, size_t n, uint32_t t, bool atomic) {
    uintptr_t end = begin + static_cast<uintptr_t>(n);
    uintptr_t a = begin;
    while (a < end) {
        uintptr_t line = a & ~(kLineBytes - 1);
        uintptr_t line_end = std::min(end, line + kLineBytes);
        LineCell* lc = line_cell_at(line);
        if (line_is_hwcc(lc)) {
            a = line_end;
            continue;
        }
        line_lock(lc);
        // The part of THIS line the store covers, which is the location an
        // atomic store is at. Taken before any byte is installed, since
        // install() overwrites what the last write recorded.
        uint32_t off = static_cast<uint32_t>(a - line);
        uint32_t len = static_cast<uint32_t>(line_end - a);
        // A store covering every byte of the line leaves nothing of a stale
        // copy to ride back out, so its line premise weakens (write_byte).
        // One range only: four stores that together cover the line are four
        // partial stores here, and stay under the ordinary premise.
        bool covers_line = off == 0 && len == kLineBytes;
        if (atomic) {
            check_atomic_line_write(lc, a, t, off, len);
        }
        Epoch E = now_epoch(t);
        while (a < line_end) {
            uintptr_t word = a & ~(kWordBytes - 1);
            uintptr_t hi = std::min(line_end, word + kWordBytes);
            ByteCell* wc = word_cell_at(word);
            bool whole = a == word && hi == word + kWordBytes;
            if (whole && cell_split(wc)) {
                // Every byte is checked and installed on its own cell first --
                // a byte another thread read has a read clock only its cell
                // holds. Only then do the eight agree again and collapse.
                ByteCell* bytes = split_array(wc);
                for (uint32_t i = 0; i < kWordBytes; ++i) {
                    write_byte(&bytes[i], lc, word + i, t, E, atomic, covers_line);
                }
                unsplit(wc);
                wc->wb = E;
                if (uint32_t* pos = write_pos_at(word)) {
                    *pos = trace_cur_pos();
                }
                cell_set_atomic(wc, atomic);
            } else if (whole) {
                write_byte(wc, lc, word, t, E, atomic, covers_line);
            } else {
                ByteCell* bytes = ensure_split(wc);
                if (bytes == nullptr) {
                    write_byte(wc, lc, word, t, E, atomic, covers_line);
                } else {
                    for (uintptr_t b = a; b < hi; ++b) {
                        write_byte(&bytes[b & (kWordBytes - 1)], lc, b, t, E, atomic, covers_line);
                    }
                }
            }
            a = hi;
        }
        // Name the location this write went to, for the next atomic store to
        // compare against.
        lc->wl_off = static_cast<uint8_t>(off);
        lc->wl_len = static_cast<uint8_t>(len > kLineBytes ? kLineBytes : len);
        line_unlock(lc);
    }
    atomic_fetch_add_u64(&g_header->write_events, n);
}

// ---- plain access rules ---------------------------------------------------------

// Read(x,t) for every byte of the range.
void rule_read(uintptr_t begin, size_t n, uint32_t t) {
    read_range(begin, n, t, /*atomic=*/false);
}

// Write(x,t) for every byte of the range, plus 𝕊_x ← 0, 𝕍_x ← ∅: a
// plain store releases nothing and ends every release sequence at the bytes
// it covers, on every machine.
void rule_write(uintptr_t begin, size_t n, uint32_t t) {
    sync_clear_range(shadow_offset(begin), shadow_offset(begin) + n); // keyed by pool offset
    write_range(begin, n, t, /*atomic=*/false);
}

} // namespace csan
