// SPDX-License-Identifier: MIT
// The rules Load and Store, their premises, and the Store update. The walk
// over lines, words and split cells is repr/walk.h.
#include "rules/rules.h"

#include "base/atomics.h"
#include "base/report.h"
#include "base/trace.h"
#include "repr/shadow.h"
#include "repr/slotmap.h"
#include "repr/walk.h"

#include <algorithm>

namespace csan {

// ---- R_x, the reads of a byte since its last write ----
// FastTrack's adaptive form. While the reads are ordered by happens-before,
// the last one follows all the others, so its epoch alone decides
// R_x ⊑ C_u for every later writer u. From the first concurrent read, R_x
// is a ReadList with each thread's latest read. Storage: repr/shadow.h.

// R_x ⊑ C_t
static inline bool rd_le(const ByteCell* x, uint32_t t) {
    return cell_rd_shared(x) ? readlist_le(cell_rd_off(x), t) : thread_vc(t).covers(x->rd);
}

// R_x ← addRead(R_x, t), FastTrack's three read cases:
//   R_x an epoch ⊑ C_t   this read follows it: this read's epoch replaces it,
//                        whichever thread made it (always so for t's own);
//   R_x an epoch ⋢ C_t   a concurrent read: both go into a ReadList;
//   R_x a ReadList       raise t's entry.
static inline void add_read(ByteCell* x, uint32_t t) {
    uint32_t now = vc_get(t, t);
    if (cell_rd_shared(x)) {
        readlist_raise(cell_rd_off(x), t, now);
    } else if (x->rd == kEpochBot || thread_vc(t).covers(x->rd)) {
        x->rd = epoch_make(t, now);
    } else {
        rd_to_list(x, t, now);
    }
}

// ---- the premises Load and Store add (formulas in rules.h) ----

bool load_acq_premise(const ByteCell* x, uint32_t t) {
    if (x->wb == kEpochBot || machine_of(epoch_tid(x->wb)) == machine_of(t)) {
        return true;
    }
    return acq_r(x, t);
}

bool store_pub_premise(const ByteCell* x, uint32_t t) {
    if (x->wb == kEpochBot || machine_of(epoch_tid(x->wb)) == machine_of(t)) {
        return true;
    }
    return cell_atomic_store(x) || pub_b(x, t);
}

bool store_acq_premise(const ByteCell* y, uint32_t t) {
    return y->wb == kEpochBot || acq_w(y, t);
}

void store_update(ByteCell* x, uint32_t t, Epoch now) {
    uint32_t m = machine_of(t);
    bool moved = x->wb != kEpochBot && machine_of(epoch_tid(x->wb)) != m;
    rd_clear(x); // R_x ↦ ⊥
    if (moved) {
        // (W'_x, P'_x) ↦ (W_x, P_x); the old P'_x's block is reused for P_x
        uint32_t p = cell_pub_off(x);
        cell_set_pub_off(x, x->pprev);
        x->pprev = p;
        x->wprev = x->wb;
    }
    slotmap_clear(cell_pub_off(x));
    slotmap_clear_other_machines(cell_acq_off(x), m);
    // A_x[t] ↦ a
    // W_x = ⊥: AllowAccess(⊥, ⊥) is not stored; init_allow_access derives it.
    if (moved) {
        SlotState st = slotmap_state_of(cell_acq_off(x), t);
        if (st == kSlotNone || st == kSlotPending) {
            if (SlotMap* a = cell_acq_get(x)) {
                uint32_t k = epoch_clock(now); // AllowAccess(now(t), now(t))
                slotmap_set(a, t, kSlotAllowAccess, k, k, trace_pos());
            }
        }
    }
    x->wb = now;
}

// ---- Load and Store on one cell (a byte, or an unsplit word); caller holds the line lock ----

// Load:  W_x ⪯ C_t  ∧  load_acq_premise(x,t)  ⟹  R_x ↦ addRead(R_x, t)
// The atomic loads (beyond the paper): W_x ⪯ C_t is discharged if the last
// access to x was atomic too, and the AcqR premise is skipped.
static void load_byte(ByteCell* x, uintptr_t addr, uint32_t t, bool atomic) {
    if (!(g_report_once && cell_reported(x))) {
        bool both_atomic = atomic && cell_atomic(x);
        bool bad = false;
        if (!both_atomic && !epoch_le(x->wb, t)) {
            report_race(CSAN_RACE_DATA_RACE_WR, addr, t, x->wb);
            bad = true;
        }
        if (!atomic && !load_acq_premise(x, t)) {
            report_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE, addr, t, x->wb);
            bad = true;
        }
        if (bad) {
            cell_set_reported(x);
        }
    }
    trace_access("READ", addr, t);
    add_read(x, t);
    cell_set_atomic(x, atomic);
}

// Store, for one byte x ∈ X:
//   W_x ⪯ C_t  ∧  R_x ⪯ C_t  ∧  store_pub_premise(x,t)  ⟹  store_update(x, now(t))
// The premise on c ∖ X is checked once per line (store_acq_premise_line).
// The atomic stores (beyond the paper): both ⪯ premises are discharged if
// the last access to x was atomic too.
// `line_bad`: the premise on c ∖ X failed for this store, which marks X.
static void store_byte(ByteCell* x, uintptr_t addr, uint32_t t, Epoch now, bool atomic,
                       bool line_bad) {
    if (!(g_report_once && cell_reported(x))) {
        bool both_atomic = atomic && cell_atomic(x);
        bool bad = line_bad;
        if (!both_atomic && !epoch_le(x->wb, t)) {
            report_race(CSAN_RACE_DATA_RACE_WW, addr, t, x->wb);
            bad = true;
        }
        if (!both_atomic && !rd_le(x, t)) {
            report_race(CSAN_RACE_DATA_RACE_RW, addr, t);
            bad = true;
        }
        if (!store_pub_premise(x, t)) {
            report_race(CSAN_RACE_WRITE_WITHOUT_PUBLISH, addr, t, x->wb);
            bad = true;
        }
        if (bad) {
            cell_set_reported(x);
        }
    }
    trace_access("WRITE", addr, t);
    store_update(x, t, now);
    set_write_pos(addr, trace_pos());
    cell_set_atomic(x, atomic);
    cell_set_atomic_store(x, atomic);
}

// The premise on c ∖ X only gets easier while no other machine stores to c:
// only such a store resets t's machine's entries of A_y, and a fence, flush
// or store of t's machine only adds them. A byte t itself stored stays
// covered: its A_y keeps an entry of t's machine with E_w ⪯ W_y ⪯ C_t (as
// long as Store's W_y ⪯ C_t held). So a thread for which it held on c holds
// it again until c's `ver` moves. One entry per line slot, per OS thread;
// the tid and g_memo_gen guard reuse across hooks and forks.
struct AcqPremiseMemo {
    const LineCell* line;
    uint32_t ver;
    uint32_t tid;
    uint32_t gen;
};
constexpr uint32_t kAcqPremiseMemoSlots = 64;
static thread_local AcqPremiseMemo t_acq_premise_memo[kAcqPremiseMemoSlots];

static AcqPremiseMemo& acq_premise_memo_slot(const LineCell* lc) {
    return t_acq_premise_memo[(reinterpret_cast<uintptr_t>(lc) / sizeof(LineCell)) %
                              kAcqPremiseMemoSlots];
}

// ∀y ∈ c ∖ X. store_acq_premise(y,t), for a store to the span: reports the
// first byte that fails, and says whether all held. Before the update; a
// store to the whole line has no such byte.
static bool store_acq_premise_line(const LineSpan& sp, uint32_t t, bool atomic) {
    const AcqPremiseMemo& m = acq_premise_memo_slot(sp.lc);
    if (m.line == sp.lc && m.ver == sp.lc->ver && m.tid == t && m.gen == g_memo_gen) {
        return true;
    }
    OutsideCell y =
        find_cell_outside(sp, [&](const ByteCell* c) { return !store_acq_premise(c, t); });
    if (y.cell == nullptr) {
        return true;
    }
    report_race(atomic ? CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE
                       : CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE,
                sp.lo, t, y.cell->wb, y.addr);
    return false;
}

// After t's store to the line: a store from another machine moves `ver`,
// and a premise that held is remembered for t.
static void note_line_store(LineCell* lc, uint32_t t, bool held) {
    uint8_t me = static_cast<uint8_t>(machine_of(t) + 1);
    if (lc->last_mach != me) {
        lc->last_mach = me;
        ++lc->ver;
    }
    if (held) {
        acq_premise_memo_slot(lc) = AcqPremiseMemo{lc, lc->ver, t, g_memo_gen};
    }
}

// The cell holding byte `a`: its own if the word is split, else the word's.
static const ByteCell* cell_at(uintptr_t a) {
    ByteCell* wc = word_cell_at(a);
    return cell_split(wc) ? &split_array(wc)[a & (kWordBytes - 1)] : wc;
}

// ---- Load and Store over a byte range ----

// Whether load_byte(x, ·, t, atomic) would change nothing and report
// nothing: no premise fails, R_x already holds t's read at `now`, and
// atomic_x already is `atomic`. Read without the line lock (load_span_noop).
// An offset is followed only after `seq` vouches for it, so it names a block
// of its class; such blocks are written only with atomics (arena.cpp).
static bool load_cell_noop(const ByteCell* x, uint32_t t, uint32_t now, bool atomic,
                           const LineCell* lc, uint32_t seq) {
    if (cell_atomic(x) != atomic) {
        return false;
    }
    if (!atomic && !(g_report_once && cell_reported(x))) {
        Epoch w = x->wb;
        if (!epoch_le(w, t) || (w != kEpochBot && machine_of(epoch_tid(w)) != machine_of(t))) {
            return false; // a finding, or the AcqR query: the locked path decides
        }
    }
    Epoch rd = x->rd;
    if (cell_rd_shared(x)) {
        return line_read_valid(lc, seq) && readlist_holds(static_cast<uint32_t>(rd), t, now);
    }
    return rd == epoch_make(t, now);
}

// Load on the span is a no-op, by a seqlock read of its line: then the
// locked Load would have found the same state and left it as it was. Words
// a read would split, and tracing, take the locked path.
static bool load_span_noop(const LineSpan& sp, uint32_t t, bool atomic) {
    if (g_trace_on || tracing()) {
        return false;
    }
    uint32_t seq = line_read_begin(sp.lc);
    if ((seq & 1) != 0) {
        return false;
    }
    uint32_t now = vc_get(t, t);
    uint64_t limit = g_header->arena_size;
    for (uintptr_t a = sp.lo; a < sp.hi;) {
        uintptr_t word = a & ~(kWordBytes - 1);
        uintptr_t hi = std::min(sp.hi, word + kWordBytes);
        const ByteCell* wc = sp.s.cell(word);
        if (!cell_split(wc)) {
            if (a != word || hi != word + kWordBytes || !load_cell_noop(wc, t, now, atomic, sp.lc, seq)) {
                return false;
            }
        } else {
            uint32_t off = cell_rd_off(wc);
            if (!line_read_valid(sp.lc, seq) || off % 8 != 0 ||
                off + kWordBytes * sizeof(ByteCell) > limit) {
                return false;
            }
            const ByteCell* bytes = static_cast<const ByteCell*>(arena_at(off));
            for (uintptr_t b = a; b < hi; ++b) {
                if (!load_cell_noop(&bytes[b - word], t, now, atomic, sp.lc, seq)) {
                    return false;
                }
            }
        }
        a = hi;
    }
    return line_read_valid(sp.lc, seq);
}

// Load for each byte; `atomic` selects the atomic loads' shadow half.
void load_range(Shadow s, uintptr_t begin, size_t n, uint32_t t, bool atomic) {
    uintptr_t end = begin + static_cast<uintptr_t>(n);
    for (uintptr_t a = begin; a < end;) {
        uintptr_t line = a & ~(kLineBytes - 1);
        LineSpan sp{s, s.line(line), line, a, std::min(end, line + kLineBytes)};
        a = sp.hi;
        if (line_is_hwcc(sp.lc) || load_span_noop(sp, t, atomic)) {
            continue;
        }
        line_lock(sp.lc);
        for_each_cell_read(sp, [&](ByteCell* x, uintptr_t b) { load_byte(x, b, t, atomic); });
        line_unlock(sp.lc);
    }
    count_add(&g_header->counts[t].reads, n);
}

// Store per line c, on the bytes X it writes there: ∀y ∈ c ∖ X the AcqW
// premise, then for each x ∈ X the byte premises and the update. A plain
// store also ends release sequences (S_x ↦ 0), on hardware-coherent lines
// too; `atomic` selects the atomic stores' shadow half, which leave S_x to
// rules_sync.cpp.
void store_range(Shadow s, uintptr_t begin, size_t n, uint32_t t, bool atomic) {
    auto end_sequences = [&](const LineSpan& sp) {
        for_each_word_in(sp, [&](ByteCell* wc, uintptr_t, uintptr_t lo, uintptr_t hi) {
            end_release_sequences(wc, sp.s.offset(lo), sp.s.offset(hi));
        });
    };
    for_each_line_span(
        s, begin, n,
        [&](const LineSpan& sp) {
            // Under CSAN_REPORT_ONCE the line premise is skipped once the store's
            // first byte was reported; skipping does not feed the memo.
            bool skip = g_report_once && cell_reported(cell_at(sp.lo));
            bool held = skip ? false : store_acq_premise_line(sp, t, atomic);
            bool line_bad = !skip && !held;
            Epoch now = now_epoch(t);
            if (!atomic) {
                end_sequences(sp);
            }
            for_each_cell_written(
                sp,
                [&](ByteCell* x, uintptr_t a) { store_byte(x, a, t, now, atomic, line_bad); },
                [&](ByteCell*, uintptr_t word) { set_write_pos(word, trace_pos()); });
            note_line_store(sp.lc, t, held);
        },
        [&](const LineSpan& sp) {
            if (!atomic) {
                line_lock(sp.lc);
                end_sequences(sp);
                line_unlock(sp.lc);
            }
        });
    count_add(&g_header->counts[t].writes, n);
}

// ---- the plain rules ----

void load_range(uintptr_t begin, size_t n, uint32_t t, bool atomic) {
    load_range(shadow_of(begin), begin, n, t, atomic);
}

void store_range(uintptr_t begin, size_t n, uint32_t t, bool atomic) {
    store_range(shadow_of(begin), begin, n, t, atomic);
}

void rule_load(Shadow s, uintptr_t begin, size_t n, uint32_t t) {
    load_range(s, begin, n, t, /*atomic=*/false);
}

// Store, plus S_x ↦ 0: a plain store ends release sequences.
void rule_store(Shadow s, uintptr_t begin, size_t n, uint32_t t) {
    store_range(s, begin, n, t, /*atomic=*/false);
}

} // namespace csan
