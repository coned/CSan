// SPDX-License-Identifier: MIT
// The shared segment: constants, state structures, process-local pointers.
// A contract between processes: every one computes the same Layout and
// refuses to attach on mismatch. Any change here means bumping kVersion.
//
// Notation (x = byte, l = line, t = thread, a = lock or atomic address):
//   C_t            thread t's vector clock; F^rel_t / F^acq_t its fence clocks
//   W_x            the last store to byte x
//   W'_x           the last store to x from a machine other than mach(W_x)
//   R_x            the reads of x since its last write
//   P_x, P'_x      publication maps of W_x, W'_x: per thread, a write-back (then fence)
//   A_x            acquisition map: per thread, a flush (then fence), or a store
//   L_a            the clock the last unlocker of lock a published
//   S_x            the release-sequence clock of atomic location x
#ifndef CSAN_LAYOUT_H
#define CSAN_LAYOUT_H

#include "csan_runtime.h" // CSAN_RACE_KIND_COUNT: the per-kind counters below
#include "repr/epoch.h"
#include "repr/slotstate.h"
#include "base/atomics.h"
#include "base/spinlock.h"

#include <cstdint>

namespace csan {

constexpr uint32_t kMagic = 0x4353414Eu; // "CSAN"
constexpr uint32_t kVersion = 19 + (sizeof(Epoch) == 8 ? 0x10000 : 0); // widths never mix
constexpr uint32_t kMaxThreads = 256; // hard cap: TID_BITS below must cover it
constexpr uint32_t kMaxMachines = 64;
constexpr uint32_t kMaxRanges = 4096;
// Watched regions beyond the pool; each is a whole mapping.
constexpr uint32_t kMaxWatchRegions = 16;

constexpr uint32_t kMaxRoots = 64;
constexpr uint64_t kLineBytes = 64;
constexpr uint64_t kWordBytes = 8;
constexpr uint64_t kDefaultCxlSize = 1u << 20; // 1 MiB
constexpr uint64_t kAlign = 64;
constexpr uint32_t kInitStateUninit = 0;
constexpr uint32_t kInitStateInitializing = 1;
constexpr uint32_t kInitStateReady = 2;

// ---- tracing (CSAN_TRACE); off costs one predicted branch per hook ----
constexpr uint32_t kStackFrames = 8;   // frames captured per event
constexpr uint32_t kMaxModules = 32;   // distinct binaries a frame can name
constexpr uint32_t kDefaultTraceDepth = 4096;      // ring entries per thread
constexpr uint32_t kDefaultTraceStacks = 1u << 16; // interned stacks

// Order must match kEventNames in trace.cpp.
enum EventKind : uint8_t {
    kEvNone = 0,
    kEvRead,
    kEvWrite,
    kEvWriteBack,
    kEvFlush,
    kEvSFence,
    kEvMFence,
    kEvLock,
    kEvUnlock,
    kEvRelease,
    kEvAcquire,
    kEvKindCount,
};

// L_a / S_x table capacity (CSAN_CLOCK_ENTRIES), one entry per distinct
// address, never reclaimed. A full table loses happens-before: false races.
constexpr uint32_t kDefaultClockEntries = 4096;

// Vector-clock width (CSAN_THREADS): every clock row holds this many entries.
constexpr uint32_t kDefaultThreads = 64;

// ---- SlotMap: P_x : Tid -> PubSt, or A_x : Tid -> AcqSt (40 bytes) ----
// Inline (full_off == 0): up to four entries; entry i present when its state
// is not kSlotNone, keyed by epoch_tid(inline_e[i]). inline_e[i] is the
// entry's epoch E (E_w for AllowAccess); inline_m[i] the clock of E_r, read only
// in AllowAccess.
// Promoted (full_off != 0): an arena block indexed by tid,
// {uint8_t st[threads]; uint32_t clk[threads]; uint32_t clk_m[threads];};
// inline fields are dead.
struct SlotMap {
    Epoch inline_e[4];
    uint32_t inline_m[4];
    uint32_t full_off;  // 0 = inline; else arena offset of the promoted block
    uint8_t inline_s;   // four 2-bit SlotStates, entry i in bits 2i..2i+1
    uint8_t pad[3];
};

// Under tracing, after an inline SlotMap: per slot, the Pos of the
// write-back/flush that recorded it and the fence that finished it.
struct SlotProv {
    uint32_t rec[4];
    uint32_t fin[4];
};

// ---- ByteCell (24 bytes; 40 with CSAN_EPOCH64) ----
// One per 8-byte word while its bytes share state. When they diverge the word
// is split: eight ByteCells in the arena, one per byte, and this cell has
// kSplitBit set, `rd` = their offset, other fields dead. A whole-word write
// collapses it back when its bytes then agree (shadow.h). Arena offsets are 8-aligned, so bits 0..2 of
// pub/acq hold flags.
// R_x is one Epoch while the reads are ordered by happens-before (the last
// one stands for all), and a ReadList from the first concurrent read.
// Every field is Rlx: load_span_noop reads them under a seqlock, not the lock.
struct ByteCell {
    Rlx<Epoch> wb;     // W_x; kEpochBot = never written
    Rlx<Epoch> rd;     // R_x: the last ordered read's Epoch (kEpochBot = none), or ReadList offset
    Rlx<uint32_t> pub; // flags | P_x SlotMap offset (0 = empty)
    Rlx<uint32_t> acq; // flags | A_x SlotMap offset (0 = empty)
    Rlx<Epoch> wprev;  // W'_x: last store to x from a machine other than mach(W_x); kEpochBot = none
    Rlx<uint32_t> pprev; // P'_x SlotMap offset (0 = empty): the publications of W'_x
};

constexpr uint32_t kRdSharedBit = 1u; // pub bit 0: `rd` is a ReadList offset
constexpr uint32_t kAtomicBit = 2u;   // pub bit 1: atomic_x (not the A_x relay)
constexpr uint32_t kSyncBit = 4u;     // pub bit 2: S_x may be set here; a plain write must end it
constexpr uint32_t kSplitBit = 1u;    // acq bit 0: word is split
constexpr uint32_t kAtomicStoreBit = 2u; // acq bit 1: the last store to x was atomic
constexpr uint32_t kReportedBit = 4u;    // acq bit 2: a finding was reported at x (CSAN_REPORT_ONCE)
constexpr uint32_t kOffMask = ~7u;    // the arena-offset part of either field

static inline bool cell_rd_shared(const ByteCell* c) {
    return (c->pub & kRdSharedBit) != 0;
}

// `rd` as an arena offset: a ReadList (rd-shared) or a split array (split).
static inline uint32_t cell_rd_off(const ByteCell* c) {
    return static_cast<uint32_t>(c->rd);
}

// atomic_x: last access was atomic. Two atomics never race, whatever the order.
static inline bool cell_atomic(const ByteCell* c) {
    return (c->pub & kAtomicBit) != 0;
}

static inline void cell_set_atomic(ByteCell* c, bool v) {
    c->pub = v ? (c->pub | kAtomicBit) : (c->pub & ~kAtomicBit);
}

// The last store to x was atomic, so the runtime wrote it back
// (push_atomic_write). Set by each store; a load leaves it.
static inline bool cell_atomic_store(const ByteCell* c) {
    return (c->acq & kAtomicStoreBit) != 0;
}

static inline void cell_set_atomic_store(ByteCell* c, bool v) {
    c->acq = v ? (c->acq | kAtomicStoreBit) : (c->acq & ~kAtomicStoreBit);
}

// A finding was reported at x; under CSAN_REPORT_ONCE its checks are skipped
// from then on, as TSan does for a racy word. Its state is still updated.
static inline bool cell_reported(const ByteCell* c) {
    return (c->acq & kReportedBit) != 0;
}

static inline void cell_set_reported(ByteCell* c) {
    c->acq |= kReportedBit;
}

static inline bool cell_split(const ByteCell* c) {
    return (c->acq & kSplitBit) != 0;
}

static inline uint32_t cell_pub_off(const ByteCell* c) {
    return c->pub & kOffMask;
}

static inline uint32_t cell_acq_off(const ByteCell* c) {
    return c->acq & kOffMask;
}

static inline void cell_set_pub_off(ByteCell* c, uint32_t off) {
    c->pub = (c->pub & ~kOffMask) | (off & kOffMask);
}

static inline void cell_set_acq_off(ByteCell* c, uint32_t off) {
    c->acq = (c->acq & ~kOffMask) | (off & kOffMask);
}

// ---- ReadList: R_x after two concurrent reads (32 bytes, chained) ----
// Cost scales with readers, not thread slots.
constexpr uint32_t kReadListEntries = 7;

// Rlx: readlist_holds reads it under a seqlock.
struct ReadList {
    Rlx<Epoch> e[kReadListEntries]; // one per reader; kEpochBot = free
    Rlx<uint32_t> next;             // next block offset, 0 = end
};

// ---- LineCell (12 bytes, one per 64-byte line) ----
// No line-level history: stores to one line are not ordered among
// themselves, and every rule reads the bytes' own state. `ver` only lets a
// thread know the Store premise on the line's other bytes still holds for it.
struct LineCell {
    uint32_t seq;      // the line lock, a seqlock: odd while held; guards this LineCell and its words' ByteCells
    uint8_t flags;     // kHwCcBit
    uint8_t last_mach; // machine of the last store to the line, plus 1; 0 = none
    uint16_t pad;
    uint32_t ver;      // +1 whenever a store comes from a machine other than last_mach
};

// Hardware-coherent line: not tracked. Set at registration, read without the
// line lock. kAlign == kLineBytes, so a line never mixes both kinds.
constexpr uint8_t kHwCcBit = 1u;

inline void line_lock(LineCell* lc) {
    uint32_t spins = 0;
    for (;;) {
        uint32_t s = __atomic_load_n(&lc->seq, __ATOMIC_RELAXED);
        if ((s & 1) == 0 &&
            __atomic_compare_exchange_n(&lc->seq, &s, s + 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            // A reader that sees any store of ours then sees seq odd (or later).
            __atomic_thread_fence(__ATOMIC_RELEASE);
            return;
        }
        if (++spins < 64) {
            __builtin_ia32_pause();
        } else {
            sched_yield();
            spins = 0;
        }
    }
}

inline void line_unlock(LineCell* lc) {
    __atomic_store_n(&lc->seq, __atomic_load_n(&lc->seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

// Seqlock reads: an even value from line_read_begin, then line_read_valid
// after reading the line's state, which holds iff no writer ran in between.
inline uint32_t line_read_begin(const LineCell* lc) {
    return __atomic_load_n(&lc->seq, __ATOMIC_ACQUIRE);
}

inline bool line_read_valid(const LineCell* lc, uint32_t s) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&lc->seq, __ATOMIC_RELAXED) == s;
}

// ---- WatchRegion: shared memory watched but not allocated by the checker ----
// Each machine maps it at its own address, so state is keyed by (id, offset),
// never by address; each process keeps its own mapping in g_regions[].
struct WatchRegion {
    uint64_t size;       // bytes, line-aligned
    uint64_t shadow_off; // start in the unified shadow space
    uint32_t id;         // program's name for it, same on every machine
    uint32_t in_use;     // 0 = free slot
};

// ---- Event (24 bytes, a ring per thread) ----
// Rlx: a report reads other threads' rings while they write them.
struct Event {
    Rlx<uint64_t> addr;  // 0 for a bare fence
    Rlx<uint32_t> stack; // interned stack id, 0 = none
    uint32_t seq;        // per-thread counter; detects an EventRef whose ring slot was reused
    Rlx<uint32_t> from;  // acquire: EventRef of the release merged, 0 = none;
                         // flush: how many of its lines took a byte acquisition
    Rlx<uint8_t> kind;   // EventKind
    Rlx<uint8_t> len;    // bytes, for an access
    Rlx<uint16_t> src;   // CSAN_SRC_*, for a write-back or flush
};

// (tid, seq). Zero = no event; seq starts at 1.
inline uint32_t event_ref(uint32_t tid, uint32_t seq) {
    return (tid << 24) | (seq & 0xFFFFFFu);
}
inline uint32_t event_ref_tid(uint32_t r) { return r >> 24; }
inline uint32_t event_ref_seq(uint32_t r) { return r & 0xFFFFFFu; }

// Interned call stack. Frames are (module << 48) | file offset, not addresses:
// each machine has its own load bias.
struct StackEntry {
    uint64_t frame[kStackFrames];
    uint32_t hash;   // 0 = this slot is free
    uint32_t pad;
};

inline uint64_t frame_pack(uint32_t module, uint64_t off) {
    return (static_cast<uint64_t>(module) << 48) | (off & 0xFFFFFFFFFFFFull);
}
inline uint32_t frame_module(uint64_t f) { return static_cast<uint32_t>(f >> 48); }
inline uint64_t frame_offset(uint64_t f) { return f & 0xFFFFFFFFFFFFull; }

struct ModuleEntry {
    char path[232];
};

struct SharedRange {
    uint64_t base;
    uint64_t end;
    uint32_t in_use;
    uint32_t reserved;
};

// ---- sync table: L_a and S_x by key ----
// Open-addressed within each stripe; the stripe lock guards its entries and
// the clocks they point to.
constexpr uint32_t kSyncStripes = 256;

enum SyncSlot : uint32_t { kSyncEmpty = 0, kSyncLive = 1, kSyncDead = 2 };

// Rlx: sync_join probes a stripe without its lock.
struct SyncEntry {
    Rlx<uint64_t> key;
    Rlx<uint32_t> state; // SyncSlot
    Rlx<uint32_t> clock; // arena offset of the clock, 0 = none
};

struct alignas(64) SyncStripe {
    SpinLock lock;
    uint32_t seq; // odd while a writer changes the stripe (sync_join)
    Rlx<uint32_t> live; // read by sync_live without the lock
    uint32_t dead;
};

enum ArenaClass {
    kClassSlotMap = 0, // sizeof(SlotMap)
    kClassSplit = 1,   // 8 x sizeof(ByteCell)
    kClassVC = 2,      // threads x 4        (an L_a or S_x clock)
    kClassFullMap = 3, // promoted SlotMap: threads states + threads E clocks + threads E_r clocks
    kClassReadList = 4, // sizeof(ReadList): R_x after two concurrent reads
    kNumArenaClasses = 5,
};

struct alignas(64) ThreadCounts {
    uint64_t reads;
    uint64_t writes;
    uint64_t races;                           // reports emitted
    uint64_t race_kind[CSAN_RACE_KIND_COUNT]; // reports by CSAN_RACE_* kind
    uint64_t arena_full;                      // arena failures after the first (SharedHeader::arena_full)
    uint64_t overflow;                        // records dropped, beside SharedHeader::overflow_count
};

// ---- SharedHeader: fixed-size metadata at offset 0 ----
struct SharedHeader {
    // Identity: attach refuses if these disagree.
    uint32_t magic;         // kMagic, "CSAN"
    uint32_t version;       // kVersion; bump on any layout change
    uint64_t segment_size;  // total bytes of the mapping
    uint32_t hosts;         // number of simulated machines
    uint32_t threads;       // width of every vector clock in this segment
    uint64_t cxl_base;      // offset of the pool within the segment
    uint64_t cxl_size;      // pool bytes

    SpinLock global_lock;   // init, tid allocation, ranges, reports
    SpinLock arena_lock;    // the arena free lists and bump pointer
    uint32_t init_state;    // kInitStateUninit / Initializing / Ready

    // Machines and threads.
    uint32_t next_tid;                          // next global thread id to hand out
    uint32_t machine_pid[kMaxMachines];         // pid that registered each machine
    uint32_t machine_registered[kMaxMachines];  // 1 once a machine has registered
    uint32_t machine_main_tid[kMaxMachines];    // each machine's main thread (csan_join)
    uint32_t machine_of_thread[kMaxThreads];    // which machine owns each thread

    // Pool allocator and registered ranges.
    uint64_t bump;                    // next free pool offset
    uint32_t range_count;             // high-water mark in ranges[]
    uint32_t reserved1;
    SharedRange ranges[kMaxRanges];

    // Watched regions: shadow above the pool's, bump-allocated from the
    // CSAN_WATCH_BYTES budget.
    uint64_t watch_size;  // shadow bytes reserved for watched regions
    uint64_t watch_bump;  // next free shadow offset within that budget
    uint32_t watch_count; // high-water mark in watch[]
    uint32_t reserved4;
    WatchRegion watch[kMaxWatchRegions];

    uint32_t clock_entries; // locations the sync table is sized for
    uint32_t reserved2;

    uint64_t roots[kMaxRoots]; // compatibility-layer roots, as pool offset + 1

    // Arena allocator.
    uint64_t arena_size;                   // bytes available
    uint64_t arena_bump;                   // next never-yet-allocated offset
    uint32_t arena_free[kNumArenaClasses]; // per-class free-list heads
    uint32_t reserved3;

    // Counters, all monotonic.

    // State dropped for lack of room. Each drop can cause a false report, so
    // this must stay zero.
    uint64_t overflow_count;

    uint64_t clock_table_full;   // an address-keyed table hit its capacity
    uint64_t arena_full;         // an arena allocation failed
    uint64_t epoch_overflow;     // a clock passed kClockBits
    uint64_t promote_count;      // SlotMaps promoted past four inline slots
    uint64_t split_count;        // words split to byte granularity
    uint64_t rd_shared_count;    // R_x grown from one epoch to a ReadList

    // Tracing; all zero when off.
    uint32_t trace_on;
    uint32_t trace_depth;              // ring entries per thread
    uint32_t trace_stacks;             // slots in the intern table
    uint32_t module_count;             // entries used in modules[]
    ModuleEntry modules[kMaxModules];
    Rlx<uint32_t> trace_seq[kMaxThreads]; // each thread's next event number
    Rlx<uint32_t> os_tid[kMaxThreads];    // the kernel tid running each thread, 0 = unknown

    // The last 64 reports, as text, for __csan_report().
    uint32_t report_head;
    char report_lines[64][256];

    // Per-thread byte counts, one cache line each.
    ThreadCounts counts[kMaxThreads];
};

// ---- Layout: area offsets, computed identically by every process ----
//   [ SharedHeader ][ ByteCell[] ][ LineCell[] ][ clock planes ][ sync table ]
//   [ arena ][ event rings ][ stacks ][ cell/line Pos ][ the pool ]
// Each area 64-byte aligned; trace areas are empty without CSAN_TRACE.
struct Layout {
    uint64_t header_size;  // SharedHeader, rounded up to kAlign
    uint64_t words;        // shadow bytes / kWordBytes: number of ByteCells
    uint64_t lines;        // shadow bytes / kLineBytes: number of LineCells
    uint64_t watch_size;   // of the shadow, how much is reserved for watched regions
    uint64_t cells_off;    // where ByteCell[] starts
    uint64_t cells_size;
    uint64_t lines_off;    // where LineCell[] starts
    uint64_t lines_size;
    uint64_t vc_off;       // where the three clock planes start
    uint64_t vc_size;
    uint64_t sync_off;     // the sync table: its stripes, then its entries
    uint64_t sync_size;
    uint32_t sync_slots;   // entries per stripe
    uint32_t clock_entries;
    uint32_t threads;      // vector-clock width for this segment
    uint64_t arena_off;
    uint64_t arena_size;
    uint64_t events_off;   // per-thread event rings; 0 when tracing is off
    uint64_t events_size;
    uint64_t stacks_off;   // the interned-stack table
    uint64_t stacks_size;
    uint64_t cellpos_off;  // Pos of each word's W_x; 0 when tracing is off
    uint64_t cellpos_size;
    uint32_t trace_depth;  // ring entries per thread, 0 when tracing is off
    uint32_t trace_stacks; // slots in the intern table
    uint64_t pool_off;     // where the CXL pool itself starts
    uint64_t total;        // segment bytes
};

inline uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

// Power of two, twice the stripe's share of clock_entries: short probes.
inline uint32_t sync_stripe_slots(uint32_t clock_entries) {
    uint32_t n = 16;
    while (static_cast<uint64_t>(n) * kSyncStripes < 2ull * clock_entries) {
        n <<= 1;
    }
    return n;
}

inline Layout compute_layout(uint64_t cxl_size, uint32_t clock_entries, uint32_t threads,
                             uint64_t arena_size, uint64_t watch_size, uint32_t trace_depth,
                             uint32_t trace_stacks) {
    Layout lay{};
    lay.header_size = align_up(sizeof(SharedHeader), kAlign);
    lay.threads = threads;
    // Shadow covers the pool, then watched regions from offset cxl_size.
    lay.watch_size = watch_size;
    uint64_t shadow_bytes = cxl_size + watch_size;
    lay.words = shadow_bytes / kWordBytes;
    lay.lines = shadow_bytes / kLineBytes;
    lay.cells_off = lay.header_size;
    lay.cells_size = align_up(lay.words * sizeof(ByteCell), kAlign);
    lay.lines_off = lay.cells_off + lay.cells_size;
    lay.lines_size = align_up(lay.lines * sizeof(LineCell), kAlign);
    lay.vc_off = lay.lines_off + lay.lines_size;
    // C_t, F^rel_t, F^acq_t planes, then a per-thread "F^rel_t set" byte.
    lay.vc_size = align_up(3ull * threads * threads * sizeof(uint32_t) + threads, kAlign);
    lay.clock_entries = clock_entries;
    lay.sync_slots = sync_stripe_slots(clock_entries);
    lay.sync_off = lay.vc_off + lay.vc_size;
    lay.sync_size = align_up(kSyncStripes * (sizeof(SyncStripe) +
                                             static_cast<uint64_t>(lay.sync_slots) * sizeof(SyncEntry)),
                             kAlign);
    lay.arena_off = lay.sync_off + lay.sync_size;
    lay.arena_size = align_up(arena_size, kAlign);
    lay.trace_depth = trace_depth;
    lay.trace_stacks = trace_stacks;
    lay.events_off = lay.arena_off + lay.arena_size;
    lay.events_size = align_up(static_cast<uint64_t>(threads) * trace_depth * sizeof(Event), kAlign);
    lay.stacks_off = lay.events_off + lay.events_size;
    lay.stacks_size = align_up(static_cast<uint64_t>(trace_stacks) * sizeof(StackEntry), kAlign);
    uint64_t pos_words = trace_depth != 0 ? lay.words : 0;
    lay.cellpos_off = lay.stacks_off + lay.stacks_size;
    lay.cellpos_size = align_up(pos_words * sizeof(uint32_t), kAlign);
    lay.pool_off = lay.cellpos_off + lay.cellpos_size;
    lay.total = lay.pool_off + cxl_size;
    return lay;
}

static_assert(kMaxThreads <= (1u << kTidBits), "tid field too narrow");

// ---- process-local pointers into the mapping, set by attach_shared_segment ----
extern SharedHeader* g_header;
extern uint8_t* g_segment;
extern uint8_t* g_pool;       // the CXL pool itself
extern Event* g_events;       // per-thread rings: g_events[tid * g_trace_depth + i]
extern StackEntry* g_stacks;  // the interned-stack table
extern uint32_t g_trace_depth;   // ring entries per thread, 0 = tracing off
extern uint32_t g_trace_stacks;  // slots in the intern table
extern ByteCell* g_cells;     // one per 8-byte pool word
extern LineCell* g_lines;     // one per 64-byte pool line
// Provenance (tracing only): a Pos is an interned stack id, 0 = none, kept
// beside W_x here and after each slot map and split array.
extern bool g_prov;
extern uint32_t* g_cell_pos;  // one per 8-byte pool word, while the word is unsplit
extern uint32_t* g_vc;        // C_t      : g_vc[tid * g_threads + other]
extern uint32_t* g_frel;      // F^rel_t  : same shape
extern uint32_t* g_facq;      // F^acq_t  : same shape
extern uint8_t* g_frel_set;   // whether F^rel_t is set, one byte per thread
extern SyncStripe* g_sync_stripes;
extern SyncEntry* g_sync;     // stripe i owns g_sync[i * g_sync_slots, (i + 1) * g_sync_slots)
extern uint32_t g_sync_slots;
extern uint8_t* g_arena;      // variable-size shadow state
extern uint32_t g_threads;    // vector-clock width for this segment
extern Rlx<uint32_t> g_machine;  // this process's machine id
extern Rlx<uint32_t> g_memo_gen; // bumped when this process binds a machine: drops the Store premise memo
extern AcqRel<bool> g_attached;  // read by every hook, set by whichever thread attaches

// ---- address -> shadow; process-local, so a lookup never reads the segment ----
// Rlx: csan_watch may rewrite a slot while other threads look addresses up.
struct Region {
    Rlx<uintptr_t> lo;
    Rlx<uintptr_t> hi;  // lo == hi: not mapped here
    Rlx<uint64_t> bias; // shadow index = address + bias

    void set(uintptr_t l, uintptr_t h, uint64_t b) {
        lo = l;
        hi = h;
        bias = b;
    }
};

constexpr uint32_t kMaxRegions = 1 + kMaxWatchRegions; // [0] the pool, [1 + i] watch[i]
extern Region g_regions[kMaxRegions];
extern uint32_t g_region_count;

// Bumped (release) after every change to g_regions; starts at 1.
extern uint32_t g_region_gen;

// Per thread, the last lookups by 4 KiB page: a page wholly inside one
// region, or wholly outside all, answers without the scan while g_region_gen
// is unchanged. idx < 0: no region.
struct RegionHit {
    uintptr_t page;
    uint32_t gen;
    int32_t idx;
};
constexpr uint32_t kRegionCacheSlots = 64;
constexpr uint32_t kRegionPageBits = 12;
extern __thread RegionHit t_region_cache[kRegionCacheSlots] __attribute__((tls_model("initial-exec")));

inline void region_changed() {
    __atomic_fetch_add(&g_region_gen, 1, __ATOMIC_RELEASE);
}

const Region* region_scan(uintptr_t a);

inline const Region* region_of(uintptr_t a) {
    uint32_t gen = __atomic_load_n(&g_region_gen, __ATOMIC_ACQUIRE);
    uintptr_t page = a >> kRegionPageBits;
    const RegionHit& h = t_region_cache[page % kRegionCacheSlots];
    if (h.page == page && h.gen == gen) {
        return h.idx < 0 ? nullptr : &g_regions[h.idx];
    }
    return region_scan(a);
}

// Is [a, a+n) CXL memory the checker tracks?
inline bool cxl_contains(uintptr_t a, size_t n) {
    const Region* r = region_of(a);
    return r != nullptr && a + n <= r->hi;
}

// One region's shadow, resolved once so a range walk indexes cells directly.
struct Shadow {
    uint64_t bias;

    uint64_t offset(uintptr_t a) const {
        return a + bias;
    }
    ByteCell* cell(uintptr_t a) const {
        return &g_cells[offset(a) / kWordBytes];
    }
    LineCell* line(uintptr_t a) const {
        return &g_lines[offset(a) / kLineBytes];
    }
};

// Defined only where cxl_contains holds.
inline Shadow shadow_of(uintptr_t a) {
    return Shadow{region_of(a)->bias};
}

inline uint64_t shadow_offset(uintptr_t a) {
    const Region* r = region_of(a);
    return r != nullptr ? a + r->bias : 0;
}

inline ByteCell* word_cell_at(uintptr_t a) {
    return shadow_of(a).cell(a);
}

inline LineCell* line_cell_at(uintptr_t a) {
    return shadow_of(a).line(a);
}

// No line lock needed: set at registration, before use, and never changes.
inline bool line_is_hwcc(const LineCell* lc) {
    return (__atomic_load_n(&lc->flags, __ATOMIC_RELAXED) & kHwCcBit) != 0;
}

// Single writer per counter, so no atomic RMW needed.
inline void count_add(uint64_t* c, uint64_t n) {
    __atomic_store_n(c, __atomic_load_n(c, __ATOMIC_RELAXED) + n, __ATOMIC_RELAXED);
}

inline uint64_t count_total(uint64_t ThreadCounts::*field) {
    uint64_t sum = 0;
    for (const ThreadCounts& c : g_header->counts) {
        sum += __atomic_load_n(&(c.*field), __ATOMIC_RELAXED);
    }
    return sum;
}

inline uint64_t race_kind_total(uint32_t kind) {
    uint64_t sum = 0;
    for (const ThreadCounts& c : g_header->counts) {
        sum += __atomic_load_n(&c.race_kind[kind], __ATOMIC_RELAXED);
    }
    return sum;
}

inline uint32_t machine_of(uint32_t tid) {
    if (tid >= kMaxThreads) {
        return UINT32_MAX;
    }
    return atomic_load_u32(&g_header->machine_of_thread[tid]);
}

} // namespace csan

#endif
