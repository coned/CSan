// SPDX-License-Identifier: MIT
// WriteBack, Flush, SFence and MFence, and the four state transformations
// they apply.
//
// A write-back or flush RECORDS an intermediate slot -- wb(E) or flushed(E) --
// and the thread's next fence FINISHES it. So that a fence need not scan every
// byte of the pool, each record is also queued in pending_queue(t), per relay kind,
// and the fence completes exactly the slots this thread queued. This is exact:
// a fence only finishes slots keyed by the fencing thread, all queued by it.
#include "rules/rules.h"

#include "base/atomics.h"
#include "base/report.h"
#include "repr/shadow.h"
#include "repr/slotmap.h"

#include <algorithm>

namespace csan {

// ---- recording ---------------------------------------------------------------

// recordWB / recordFlush on one map, queueing a new record for the fence.
// `m` is null when the arena could not supply the map.
static void record(SlotMap* m, uintptr_t addr, uint32_t t, Epoch E, PendingKind kind) {
    if (m != nullptr && slotmap_record(m, t, E)) {
        pending_queue(t, kind).push_back(Pending{addr, E});
    }
}

// WB_{t,ℓ,E}(σ):
//   ℙ^L_ℓ ← recordWB(ℙ^L_ℓ, t, E)   if mach(t) = mach(thr(W^L_ℓ))  ∧  W^L_ℓ ⊑ ℂ_t
//   ℙ^B_x ← recordWB(ℙ^B_x, t, E)   for each x ∈ ℓ,
//                                    if mach(t) = mach(thr(W^L_ℓ))  ∧  W^B_x ⊑ ℂ_t
// Only the machine currently owning the dirty line can publish it; on any
// other machine the write-back is a no-op, not an error.
void wb_state(LineCell* lc, uintptr_t line, uint32_t t, Epoch E) {
    if (lc->wl == kEpochBot || machine_of(epoch_tid(lc->wl)) != machine_of(t)) {
        return;
    }
    if (epoch_le(lc->wl, t)) {
        record(slotmap_get(&lc->pub), line, t, E, kPendingPubLine);
    }
    for_each_byte_cell(line, [&](ByteCell* x, uintptr_t addr) {
        bool ok = x->wb != kEpochBot && epoch_le(x->wb, t);
        if (ok) {
            record(cell_pub_get(x), addr, t, E, kPendingPubByte);
        }
        if (trace_hit(addr)) {
            trace_event("WB", addr, t, ok ? "byte publication recorded" : "nothing to publish");
        }
    });
}

// FL_{t,ℓ,E}(σ): first the possible write-back, then, in that state,
//   𝔸^L_ℓ ← recordFlush(𝔸^L_ℓ, t, E)   if Pub^L(ℓ, ℂ_t)
//   𝔸^B_x ← recordFlush(𝔸^B_x, t, E)   for each x ∈ ℓ, if Pub^B(x, ℂ_t)
// A flush records an acquisition only where the producer's publication
// already happens-before it.
FlushResult fl_state(LineCell* lc, uintptr_t line, uint32_t t, Epoch E) {
    FlushResult r{};
    wb_state(lc, line, t, E);
    if (pub_ok(lc->pub, t)) {
        record(slotmap_get(&lc->acq), line, t, E, kPendingAcqLine);
        r.line = true;
    }
    for_each_byte_cell(line, [&](ByteCell* x, uintptr_t addr) {
        bool ok = pub_ok(cell_pub_off(x), t);
        if (ok) {
            record(cell_acq_get(x), addr, t, E, kPendingAcqByte);
            r.byte = true;
        }
        if (trace_hit(addr)) {
            trace_event("FLUSH", addr, t, ok ? "byte acquisition recorded"
                                             : "no acquisition: producer's publication not in this clock");
        }
    });
    return r;
}

// ---- finishing ---------------------------------------------------------------

static void finish_in(uint32_t off, uint32_t t, Epoch E) {
    SlotMap* m = slotmap_at(off);
    if (m != nullptr) {
        slotmap_finish(m, t, E);
    }
}

// finishPub or finishAcq, as `kind` says, over every record t queued.
//
// A byte record names the cell it was made on. If the word has been split
// since, ensure_split copied the record into all eight byte cells, so every
// one of them is finished; if it was split then and has been collapsed since,
// the Install that collapsed it emptied the maps and there is nothing left.
static void finish_pending(uint32_t t, PendingKind kind, Epoch E) {
    std::vector<Pending>& queue = pending_queue(t, kind);
    for (const Pending& p : queue) {
        LineCell* lc = line_cell_at(p.addr);
        line_lock(lc);
        if (kind == kPendingPubLine) {
            finish_in(lc->pub, t, E);
        } else if (kind == kPendingAcqLine) {
            finish_in(lc->acq, t, E);
        } else {
            bool pub = kind == kPendingPubByte;
            ByteCell* wc = word_cell_at(p.addr);
            if (!cell_split(wc)) {
                finish_in(pub ? cell_pub_off(wc) : cell_acq_off(wc), t, E);
            } else {
                ByteCell* bytes = split_array(wc);
                for (uint32_t i = 0; i < kWordBytes; ++i) {
                    finish_in(pub ? cell_pub_off(&bytes[i]) : cell_acq_off(&bytes[i]), t, E);
                }
            }
        }
        line_unlock(lc);
    }
    queue.clear();
}

// SF_{t,E}(σ): finishPub on every ℙ^B_x and ℙ^L_ℓ, finishAcq on every 𝔸^L_ℓ.
// An sfence completes publication and line acquisition. It never changes
// 𝔸^B_x: only an mfence completes the byte acquisition a later read needs.
void sf_state(uint32_t t, Epoch E) {
    finish_pending(t, kPendingPubByte, E);
    finish_pending(t, kPendingPubLine, E);
    finish_pending(t, kPendingAcqLine, E);
}

// MF_{t,E}(σ): SF_{t,E}, and finishAcq on every 𝔸^B_x.
void mf_state(uint32_t t, Epoch E) {
    sf_state(t, E);
    finish_pending(t, kPendingAcqByte, E);
}

// ---- write-back, flush and fence rules -------------------------------------------
//
// Each rule records its current epoch E = now(t) and THEN increments the
// thread's clock. Flush performs its write-back step inside fl_state, not
// through rule_writeback, so a flush ticks once.

// Visit each line touched by [begin, begin+n), holding its lock. A hardware-
// coherent line keeps no write-back or flush state, so it is skipped; the
// thread's clock still ticks for the event in the caller.
template <class F>
static void for_each_line_locked(uintptr_t begin, size_t n, F f) {
    uintptr_t end = begin + static_cast<uintptr_t>(n);
    for (uintptr_t line = begin & ~(kLineBytes - 1); line < end; line += kLineBytes) {
        LineCell* lc = line_cell_at(line);
        if (line_is_hwcc(lc)) {
            continue;
        }
        line_lock(lc);
        f(lc, line);
        line_unlock(lc);
    }
}

// WriteBack:  e = wb(ℓ,t),  E = now(t)   ⟹   WB_{t,ℓ,E}(σ)[ℂ_t[t] ↦ ℂ_t[t]+1]
void rule_writeback(uintptr_t begin, size_t n, uint32_t t) {
    Epoch E = now_epoch(t);
    for_each_line_locked(begin, n, [&](LineCell* lc, uintptr_t line) { wb_state(lc, line, t, E); });
    tick(t);
}

// Flush:  e = fl(ℓ,t),  E = now(t)   ⟹   FL_{t,ℓ,E}(σ)[ℂ_t[t] ↦ ℂ_t[t]+1]
uint32_t rule_flush(uintptr_t begin, size_t n, uint32_t t) {
    Epoch E = now_epoch(t);
    uint32_t lines = 0, bytes = 0;
    for_each_line_locked(begin, n, [&](LineCell* lc, uintptr_t line) {
        FlushResult r = fl_state(lc, line, t, E);
        lines += r.line ? 1 : 0;
        bytes += r.byte ? 1 : 0;
    });
    tick(t);
    return flush_verdict(lines, bytes);
}

// SFence:  e = sf(t),  E = now(t)   ⟹   SF_{t,E}(σ)[ℂ_t[t] ↦ ℂ_t[t]+1]
void rule_sfence(uint32_t t) {
    Epoch E = now_epoch(t);
    sf_state(t, E);
    tick(t);
}

// MFence:  e = mf(t),  E = now(t)   ⟹   MF_{t,E}(σ)[ℂ_t[t] ↦ ℂ_t[t]+1]
void rule_mfence(uint32_t t) {
    Epoch E = now_epoch(t);
    mf_state(t, E);
    tick(t);
}

} // namespace csan
