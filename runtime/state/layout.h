// SPDX-License-Identifier: MIT
// The shape of the shared segment: its constants, the state structures it
// holds, and the process-local pointers into it.
//
// Every process computes the same Layout from the same environment and refuses
// to attach if the header disagrees, so these definitions are a contract
// between processes, not merely between translation units. Changing any of
// them means bumping kVersion.
//
// Notation for the checker's state (x = a byte, l = a line, t = a thread,
// a = a lock or atomic address):
//   C_t            thread t's vector clock; F^rel_t / F^acq_t its fence clocks
//   W^B_x, W^L_l   the last write to byte x / to any byte of line l
//   R_x            the reads of x since its last write
//   P^B_x, P^L_l   publication maps: per thread, a write-back (then fence)
//   A^B_x, A^L_l   acquisition maps: per thread, a flush (then fence)
//   L_a            the clock the last unlocker of lock a published
//   S_x            the release-sequence clock of atomic location x
#ifndef CSAN_LAYOUT_H
#define CSAN_LAYOUT_H

#include "csan_runtime.h" // CSAN_RACE_KIND_COUNT: the per-kind counters below
#include "repr/epoch.h"
#include "repr/slotstate.h"
#include "base/spinlock.h"

#include <cstdint>

namespace csan {

constexpr uint32_t kMagic = 0x4353414Eu; // "CSAN"
constexpr uint32_t kVersion = 11;
constexpr uint32_t kMaxThreads = 256; // hard cap: TID_BITS below must cover it
constexpr uint32_t kMaxMachines = 64;
constexpr uint32_t kMaxRanges = 4096;
// Watched regions beyond the pool. A region is a whole mapping, not an
// allocation inside one.
constexpr uint32_t kMaxWatchRegions = 16;

constexpr uint32_t kMaxRoots = 64;
constexpr uint64_t kLineBytes = 64;
constexpr uint64_t kWordBytes = 8;
constexpr uint64_t kDefaultCxlSize = 1u << 20; // 1 MiB
constexpr uint64_t kAlign = 64;
constexpr uint32_t kInitStateUninit = 0;
constexpr uint32_t kInitStateInitializing = 1;
constexpr uint32_t kInitStateReady = 2;

// ---- tracing (CSAN_TRACE) --------------------------------------------------
//
// Off by default; off costs one predicted branch per hook.
constexpr uint32_t kStackFrames = 8;   // frames captured per event
constexpr uint32_t kMaxModules = 32;   // distinct binaries a frame can name
constexpr uint32_t kDefaultTraceDepth = 4096;      // ring entries per thread
constexpr uint32_t kDefaultTraceStacks = 1u << 16; // interned stacks

// What an Event records. The order is the order of kEventNames in trace.cpp.
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

// Capacity of the address-keyed L_a and S_x tables; override with
// CSAN_CLOCK_ENTRIES. One entry per distinct lock or release-store address,
// never reclaimed. A full table records no further happens-before and the
// checker starts reporting false races (tests/m1_t2_cas_lock_many/).
constexpr uint32_t kDefaultClockEntries = 4096;

// Thread slots every vector clock is sized for; override with CSAN_THREADS.
// Each clock costs 4 bytes per slot and merge() is linear in it, so size this
// to the run rather than to kMaxThreads.
constexpr uint32_t kDefaultThreads = 64;

// ---- SlotMap: Tid -> PSlot, or Tid -> ASlot -------------------------------
//
// 24 bytes.
//
//   off  size  field       contents
//   ---  ----  ----------  --------------------------------------------------
//     0    16  inline_e[4] four packed Epochs, one per entry
//    16     4  full_off    0 = inline mode; else an arena offset
//    20     1  inline_s    four 2-bit SlotStates, entry i in bits 2i..2i+1
//    21     3  pad         unused; brings the struct to a 4-byte multiple so
//                          an array of them stays naturally aligned
//   ---  ----
//         24
//
// Two modes, never a mixture:
//
//   full_off == 0   INLINE. Up to four entries live in inline_e[] and
//                   inline_s. Entry i is present when its state is not
//                   kSlotBot, and its key is epoch_tid(inline_e[i]).
//
//   full_off != 0   PROMOTED. An arena block of `threads` entries indexed
//                   DIRECTLY BY TID holds the map, and inline_e/inline_s are
//                   dead. The block is {uint8_t st[threads]; uint32_t
//                   clk[threads];} -- see fullmap_states / fullmap_clocks.
//                   Entry u's epoch is epoch_make(u, clk[u]). No tid is
//                   stored: the key always equals the epoch's tid.
struct SlotMap {
    Epoch inline_e[4];
    uint32_t full_off;
    uint8_t inline_s;
    uint8_t pad[3];
};

// Trails an inline SlotMap under tracing: for slot i, the Pos of the
// write-back or flush that recorded it and of the fence that finished it.
struct SlotProv {
    uint32_t rec[4];
    uint32_t fin[4];
};

// ---- ByteCell -------------------------------------------------------------
//
// 16 bytes. One per 8-byte pool word while all eight bytes carry equal state;
// when they diverge the word is SPLIT and eight of these live in the arena, one
// per byte. Every ^B component is logically per byte.
//
//   off  size  field  contents
//   ---  ----  -----  -------------------------------------------------------
//     0     4  wb     W^B_x, a packed Epoch. kEpochBot = never written.
//     4     4  rd     R_x: a packed Epoch, or a ReadList offset (see below)
//     8     4  pub    flags + P^B_x SlotMap offset
//    12     4  acq    flags + A^B_x SlotMap offset
//   ---  ----
//         16
//
// Arena offsets are 8-byte aligned, so bits 0..2 of `pub` and `acq` are always
// zero and carry flags instead:
//
//   pub   bit 0  kRdSharedBit  `rd` is a ReadList offset, not an Epoch
//         bit 1  kAtomicBit    atomic_x: was the last access to this byte
//                              atomic (NOT the A^B_x relay, which is `acq`)
//         bits 3+              offset of the P^B_x SlotMap, 0 = the empty map
//
//   acq   bit 0  kSplitBit     this word is SPLIT (see below)
//         bits 3+              offset of the A^B_x SlotMap, 0 = the empty map
//
// R_x is the threads that have read the byte since its last write, each with
// the clock of its latest read. It is only upserted, scanned and reset, never
// indexed by thread, so it is stored two ways:
//
//   rd-shared clear  at most one reader, held as the packed Epoch c@t itself.
//                    kEpochBot = no reader.
//   rd-shared set    two or more readers: `rd` is the arena offset of a
//                    ReadList holding one Epoch per reader.
//
// SPLIT is a representation detail. An access covering part of a word makes
// its bytes diverge, so eight ByteCells are allocated in the arena, each a copy
// of the word's state, and THIS cell becomes a pointer to them: kSplitBit set,
// `rd` holding the array's offset, wb/pub/acq dead. A write covering the whole
// word collapses it back (shadow.h).
struct ByteCell {
    Epoch wb;
    uint32_t rd;
    uint32_t pub;
    uint32_t acq;
};

constexpr uint32_t kRdSharedBit = 1u; // ByteCell::pub bit 0
constexpr uint32_t kAtomicBit = 2u;   // ByteCell::pub bit 1
constexpr uint32_t kSplitBit = 1u;    // ByteCell::acq bit 0
constexpr uint32_t kOffMask = ~7u;    // the arena-offset part of either field

static inline bool cell_rd_shared(const ByteCell* c) {
    return (c->pub & kRdSharedBit) != 0;
}

// atomic_x: whether the last access to this byte was atomic. Two atomic
// accesses to one location never race, however weak their orderings. (Not the
// A^B_x acquisition map, which is the `acq` field.)
static inline bool cell_atomic(const ByteCell* c) {
    return (c->pub & kAtomicBit) != 0;
}

static inline void cell_set_atomic(ByteCell* c, bool v) {
    c->pub = v ? (c->pub | kAtomicBit) : (c->pub & ~kAtomicBit);
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

// ---- ReadList: R_x with two or more readers -------------------------------
//
// 32 bytes, chained. One Epoch per thread that has read the byte since its
// last write, kEpochBot in unused entries; a full block links to another.
// Scans and upserts walk the chain, so the cost is the number of readers, not
// the number of thread slots. Seven readers fit in one block.
//
//   off  size  field  contents
//   ---  ----  -----  -------------------------------------------------------
//     0    28  e[7]   Epochs, one per reader; kEpochBot = free entry
//    28     4  next   offset of the next block, 0 = end of the chain
//   ---  ----
//         32
constexpr uint32_t kReadListEntries = 7;

struct ReadList {
    Epoch e[kReadListEntries];
    uint32_t next;
};

// ---- LineCell -------------------------------------------------------------
//
// 16 bytes, one per 64-byte pool line.
//
//   off  size  field  contents
//   ---  ----  -----  -------------------------------------------------------
//     0     4  wl     W^L_l, a packed Epoch: the last write to ANY byte of the
//                     line. kEpochBot = never written.
//     4     4  pub    P^L_l SlotMap offset in the arena, 0 = the empty map
//     8     4  acq    A^L_l SlotMap offset, 0 = the empty map
//    12     1  lock   spin lock guarding this line's LineCell and the eight
//                     ByteCells of the words inside it. 0 = free, 1 = held.
//    13     1  wl_off byte offset WITHIN the line of the last write's first
//                     byte, and
//    14     1  wl_len how many bytes it covered (0 = never written). Together
//                     they name the LOCATION of the last write, which
//                     check_atomic_line_write needs: two atomic stores to one
//                     location are the same bytes, two to different locations
//                     on one line are not.
//    15     1  flags  kHwCcBit: hardware-coherent, not checked. Set at
//                     registration, read without the line lock.
//   ---  ----
//         16
struct LineCell {
    Epoch wl;
    uint32_t pub;
    uint32_t acq;
    uint8_t lock;
    uint8_t wl_off;
    uint8_t wl_len;
    uint8_t flags;
};

// LineCell::flags bit 0: the program declared this line hardware-coherent, so
// none of its state is tracked. kAlign is kLineBytes, so a line never carries
// allocations of both kinds.
constexpr uint8_t kHwCcBit = 1u;

// Per-line lock. LineCell::lock is one byte so the cell stays at 16.
inline void line_lock(LineCell* lc) {
    uint32_t spins = 0;
    for (;;) {
        if (__atomic_exchange_n(&lc->lock, 1, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
        ++spins;
        if (spins < 64) {
            __builtin_ia32_pause();
        } else {
            sched_yield();
            spins = 0;
        }
    }
}

inline void line_unlock(LineCell* lc) {
    __atomic_store_n(&lc->lock, 0, __ATOMIC_RELEASE);
}


// ---- WatchRegion: shared memory the checker watches but does not own -------
//
// Every machine maps the region at its own address, so state is keyed by
// (id, offset within the region) and never by address. Each process records its
// own mapping address in g_watch_base[]; only the identity and the shadow index
// are shared.
//
// 24 bytes.
struct WatchRegion {
    uint64_t size;       // bytes, line-aligned
    uint64_t shadow_off; // where this region starts in the unified shadow space
    uint32_t id;         // the program's name for the region, equal on every machine
    uint32_t in_use;     // 0 = this slot is free
};

// ---- Event: one thing a thread did, with the stack that did it -------------
//
// 24 bytes, in a ring per thread. `seq` counts that thread's own events and is
// never printed; it is what tells an EventRef whose ring slot has been reused
// (history exhausted) from one still holding the event it names.
struct Event {
    uint64_t addr;  // what the event is about; 0 for a bare fence
    uint32_t stack; // interned stack id, 0 = none captured
    uint32_t seq;   // this thread's own event counter
    uint32_t from;  // EventRef of the release an acquire merged from, 0 = none;
                    // for a flush, its flush_verdict()
    uint8_t kind;   // EventKind
    uint8_t len;    // bytes, for an access
    uint16_t src;   // CSAN_SRC_*: which construct, for a write-back or flush
};

// What a flush recorded, kept in its event so a report can say it: how many of
// its lines took a line acquisition (low half) and how many a byte one (high).
inline uint32_t flush_verdict(uint32_t lines, uint32_t bytes) {
    return ((bytes > 0xFFFFu ? 0xFFFFu : bytes) << 16) | (lines > 0xFFFFu ? 0xFFFFu : lines);
}
inline uint32_t flush_verdict_lines(uint32_t v) { return v & 0xFFFFu; }
inline uint32_t flush_verdict_bytes(uint32_t v) { return v >> 16; }

// A reference to one event: (tid, seq). Zero is "no event" -- seq starts at 1.
inline uint32_t event_ref(uint32_t tid, uint32_t seq) {
    return (tid << 24) | (seq & 0xFFFFFFu);
}
inline uint32_t event_ref_tid(uint32_t r) { return r >> 24; }
inline uint32_t event_ref_seq(uint32_t r) { return r & 0xFFFFFFu; }

// An interned call stack. Frames are (module index << 48) | file offset, not
// runtime addresses: each machine has its own load bias and stacks are
// symbolized after the fact.
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

// ---- ClockEntry: one entry of the L_a table --------------------------------
//
// Variable length: `threads` clocks follow the header, so an array of these is
// walked with a run-time stride (clock_entry / clock_entry_vc).
//
//   off  size       field     contents
//   ---  ---------  --------  -----------------------------------------------
//     0          8  key       the lock's key (sync_key: pool offset, or address+machine)
//     8          4  in_use    0 = this table slot is free
//    12          4  reserved  unused; aligns the clocks that follow
//    16  threads*4  vc[]      L_a: the clock the last unlocker published
//   ---  ---------
//       16+threads*4, rounded up to 8
struct ClockEntry {
    uint64_t key;
    uint32_t in_use;
    uint32_t reserved;
    // uint32_t vc[threads];
};

// ---- S_x: the release-sequence clock of an atomic location -----------------
//
// One entry per location with a live release sequence, holding the clock at its
// head. Not keyed by machine. `key` is a pool offset
// inside the pool and (address, machine) outside it; see sync_key.
//
// 16 bytes.
//
//   off  size  field    contents
//   ---  ----  -------  -----------------------------------------------------
//     0     8  key      the location's key
//     8     4  in_use   0 = this table slot is free
//    12     4  s_off    arena offset of S_x, 0 = the zero clock
//   ---  ----
//         16
struct SyncEntry {
    uint64_t key;
    uint32_t in_use;
    uint32_t s_off;
};

enum ArenaClass {
    kClassSlotMap = 0, // sizeof(SlotMap)
    kClassSplit = 1,   // 8 x sizeof(ByteCell)
    kClassVC = 2,      // threads x 4        (an S_x clock)
    kClassFullMap = 3, // promoted SlotMap: threads states + threads clocks
    kClassReadList = 4, // sizeof(ReadList): R_x with two or more readers
    kNumArenaClasses = 5,
};

// ---- SharedHeader: the segment's fixed-size metadata, at offset 0 ----------
//
// Everything variable-size follows it; see Layout. Grouped by purpose.
struct SharedHeader {
    // Identity. Every process recomputes the layout from the same environment
    // and refuses to attach if these disagree.
    uint32_t magic;         // kMagic, "CSAN"
    uint32_t version;       // kVersion; bump on any layout change
    uint64_t segment_size;  // total bytes of the mapping
    uint32_t hosts;         // number of simulated machines
    uint32_t threads;       // width of every vector clock in this segment
    uint64_t cxl_base;      // offset of the pool within the segment
    uint64_t cxl_size;      // pool bytes

    SpinLock global_lock;   // init, tid allocation, ranges, both address-keyed
                            //   tables, reports, atomic critical sections
    SpinLock arena_lock;    // the arena free lists and bump pointer
    uint32_t init_state;    // kInitStateUninit / Initializing / Ready

    // Machines and threads.
    uint32_t next_tid;                          // next global thread id to hand out
    uint32_t machine_pid[kMaxMachines];         // pid that registered each machine
    uint32_t machine_registered[kMaxMachines];  // 1 once a machine has registered
    uint32_t machine_main_tid[kMaxMachines];    // thread id of each machine's main thread,
                                                //   which csan_join joins with
    uint32_t machine_of_thread[kMaxThreads];    // which machine owns each thread

    // Pool allocator and the table of registered regions.
    uint64_t bump;                    // next free pool offset
    uint32_t range_count;             // high-water mark in ranges[]
    uint32_t reserved1;
    SharedRange ranges[kMaxRanges];

    // Shared regions the checker watches but did not allocate. Shadow for them
    // sits above the pool's in the same flat arrays, bump-allocated out of the
    // budget every process computed identically (CSAN_WATCH_BYTES).
    uint64_t watch_size;  // shadow bytes reserved for watched regions
    uint64_t watch_bump;  // next free shadow offset within that budget
    uint32_t watch_count; // high-water mark in watch[]
    uint32_t reserved4;
    WatchRegion watch[kMaxWatchRegions];

    // The two address-keyed tables, both capped at clock_entries.
    uint32_t lock_count;    // entries used in the L_a table
    uint32_t release_count; // entries used in the (address, machine) S/V table
    uint32_t clock_entries; // capacity of each
    uint32_t reserved2;
    // The S/V table's hash index (sync.cpp): its free list, as entry index + 1,
    // and the live and tombstoned slot counts.
    uint32_t sync_free_head;
    uint32_t sync_live;
    uint32_t sync_tombstones;
    uint32_t reserved5;

    uint64_t roots[kMaxRoots]; // compatibility-layer roots, as pool offset + 1

    // Arena allocator.
    uint64_t arena_size;                   // bytes available
    uint64_t arena_bump;                   // next never-yet-allocated offset
    uint32_t arena_free[kNumArenaClasses]; // per-class free-list heads
    uint32_t reserved3;

    // Counters, all monotonic.
    uint64_t read_events;   // bytes read through the checker
    uint64_t write_events;  // bytes written
    uint64_t race_events;   // reports emitted

    // The same reports split by kind (CSAN_RACE_* in csan_runtime.h), so a
    // test can assert WHICH rule fired and not merely that something did.
    uint64_t race_kind[CSAN_RACE_KIND_COUNT];

    // State the runtime could not store: a publication, acquisition or read
    // record with no room, or an arena allocation that failed. Dropping one
    // makes a later visibility query answer "no" when the truth is "yes", a
    // FALSE report, so this must stay zero.
    uint64_t overflow_count;

    uint64_t clock_table_full;   // an address-keyed table hit its capacity
    uint64_t arena_full;         // an arena allocation failed
    uint64_t epoch_overflow;     // a clock passed kClockBits
    uint64_t promote_count;      // SlotMaps promoted past four inline slots
    uint64_t split_count;        // words split to byte granularity
    uint64_t rd_shared_count;    // R_x grown from one epoch to a ReadList

    // Tracing. All zero and all arrays absent from the layout when off.
    uint32_t trace_on;
    uint32_t trace_depth;              // ring entries per thread
    uint32_t trace_stacks;             // slots in the intern table
    uint32_t module_count;             // entries used in modules[]
    ModuleEntry modules[kMaxModules];
    uint32_t trace_seq[kMaxThreads];   // each thread's next event number
    uint32_t os_tid[kMaxThreads];      // the kernel tid running each thread, 0 = unknown

    // The last 64 reports, as text, for __csan_report().
    uint32_t report_head;
    char report_lines[64][256];
};

// ---- Layout: where each area starts, computed identically by every process --
//
// The segment is laid out in this order, each area 64-byte aligned:
//
//   [ SharedHeader ][ ByteCell[] ][ LineCell[] ][ clock planes ]
//   [ L_a table ][ S/V table ][ arena ][ event rings ][ stacks ][ the pool ]
//
// "clock planes" is three `threads` x `threads` matrices -- C_t, F^rel_t,
// F^acq_t -- then one byte per thread saying whether F^rel_t is set, since it
// is a vector clock OR bottom. Event rings and stacks are empty without
// CSAN_TRACE.
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
    uint64_t clocks_off;   // where the L_a table starts; the S/V table follows
    uint64_t clocks_size;  // both tables together
    uint64_t clock_stride; // bytes per ClockEntry, including its clocks
    uint32_t clock_entries;// capacity of each table
    uint32_t threads;      // vector-clock width for this segment
    uint64_t arena_off;
    uint64_t arena_size;
    uint64_t events_off;   // per-thread event rings; 0 when tracing is off
    uint64_t events_size;
    uint64_t stacks_off;   // the interned-stack table
    uint64_t stacks_size;
    uint64_t cellpos_off;  // Pos of each word's W^B_x; 0 when tracing is off
    uint64_t cellpos_size;
    uint64_t linepos_off;  // Pos of each line's W^L_l; 0 when tracing is off
    uint64_t linepos_size;
    uint32_t trace_depth;  // ring entries per thread, 0 when tracing is off
    uint32_t trace_stacks; // slots in the intern table
    uint64_t pool_off;     // where the CXL pool itself starts
    uint64_t total;        // segment bytes
};

inline uint64_t align_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}

// Slots in the S/V table's open-addressed index: a power of two, at least
// twice the table, so a probe stays short.
inline uint64_t sync_index_slots(uint32_t clock_entries) {
    uint64_t n = 64;
    while (n < 2ull * clock_entries) {
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
    // Shadow covers the pool then the watched-region budget, so a pool address
    // indexes at its own offset and a watched region starts at cxl_size. The
    // pool itself is only cxl_size; watched memory is mapped by the program.
    lay.watch_size = watch_size;
    uint64_t shadow_bytes = cxl_size + watch_size;
    lay.words = shadow_bytes / kWordBytes;
    lay.lines = shadow_bytes / kLineBytes;
    lay.cells_off = lay.header_size;
    lay.cells_size = align_up(lay.words * sizeof(ByteCell), kAlign);
    lay.lines_off = lay.cells_off + lay.cells_size;
    lay.lines_size = align_up(lay.lines * sizeof(LineCell), kAlign);
    lay.vc_off = lay.lines_off + lay.lines_size;
    // Three planes of `threads` rows: C_t, F^rel_t, F^acq_t; then one byte per
    // thread recording whether F^rel_t is set (it may be bottom).
    lay.vc_size = align_up(3ull * threads * threads * sizeof(uint32_t) + threads, kAlign);
    lay.clock_entries = clock_entries;
    lay.clock_stride = align_up(sizeof(ClockEntry) + static_cast<uint64_t>(threads) * 4, 8);
    lay.clocks_off = lay.vc_off + lay.vc_size;
    // One address-keyed clock table (L_a) plus the (addr, machine) sync table.
    // Then the S/V table's hash index: sync_index_slots(clock_entries) slots.
    lay.clocks_size = align_up(static_cast<uint64_t>(clock_entries) * lay.clock_stride +
                                   static_cast<uint64_t>(clock_entries) * sizeof(SyncEntry) +
                                   sync_index_slots(clock_entries) * sizeof(uint32_t),
                               kAlign);
    lay.arena_off = lay.clocks_off + lay.clocks_size;
    lay.arena_size = align_up(arena_size, kAlign);
    lay.trace_depth = trace_depth;
    lay.trace_stacks = trace_stacks;
    lay.events_off = lay.arena_off + lay.arena_size;
    lay.events_size = align_up(static_cast<uint64_t>(threads) * trace_depth * sizeof(Event), kAlign);
    lay.stacks_off = lay.events_off + lay.events_size;
    lay.stacks_size = align_up(static_cast<uint64_t>(trace_stacks) * sizeof(StackEntry), kAlign);
    // Positions ride beside the cells only under tracing.
    uint64_t pos_words = trace_depth != 0 ? lay.words : 0;
    uint64_t pos_lines = trace_depth != 0 ? lay.lines : 0;
    lay.cellpos_off = lay.stacks_off + lay.stacks_size;
    lay.cellpos_size = align_up(pos_words * sizeof(uint32_t), kAlign);
    lay.linepos_off = lay.cellpos_off + lay.cellpos_size;
    lay.linepos_size = align_up(pos_lines * sizeof(uint32_t), kAlign);
    lay.pool_off = lay.linepos_off + lay.linepos_size;
    lay.total = lay.pool_off + cxl_size;
    return lay;
}

static_assert(kMaxThreads <= (1u << kTidBits), "tid field too narrow");

// ---- process-local pointers into the mapping --------------------------------
//
// Set once by attach_shared_segment. Defined in globals.cpp.

extern SharedHeader* g_header;
extern uint8_t* g_segment;
extern uint8_t* g_pool;       // the CXL pool itself
extern Event* g_events;       // per-thread rings: g_events[tid * g_trace_depth + i]
extern StackEntry* g_stacks;  // the interned-stack table
extern uint32_t g_trace_depth;   // ring entries per thread, 0 = tracing off
extern uint32_t g_trace_stacks;  // slots in the intern table
// This process's own mapping address for each watched region, indexed as
// g_header->watch[] is. Process-local by nature: the whole point of a watched
// region is that the address differs per machine. 0 = this process has not
// mapped that region.
extern uintptr_t g_watch_base[kMaxWatchRegions];
extern ByteCell* g_cells;     // one per 8-byte pool word
extern LineCell* g_lines;     // one per 64-byte pool line
// Provenance: which code put the state there. A Pos is an interned stack id
// (0 = none), kept only under tracing (g_prov), beside the state it explains:
// g_cell_pos/g_line_pos for W^B_x/W^L_l, and a trailing block in each slot
// map and split array (repr/slotmap.h, repr/shadow.h).
extern bool g_prov;
extern uint32_t* g_cell_pos;  // one per 8-byte pool word, while the word is unsplit
extern uint32_t* g_line_pos;  // one per 64-byte pool line
extern uint32_t* g_vc;        // C_t      : g_vc[tid * g_threads + other]
extern uint32_t* g_frel;      // F^rel_t  : same shape
extern uint32_t* g_facq;      // F^acq_t  : same shape
extern uint8_t* g_frel_set;   // whether F^rel_t is set, one byte per thread
extern uint8_t* g_lock_clocks;// the L_a table, walked with g_clock_stride
extern SyncEntry* g_sync;     // the (address, machine) S/V table
extern uint32_t* g_sync_index; // its hash index: entry index + 1, 0 empty
extern uint64_t g_sync_index_mask;
extern uint8_t* g_arena;      // variable-size shadow state
extern uint32_t g_threads;    // vector-clock width for this segment
extern uint64_t g_clock_stride;
extern uint32_t g_machine;    // this process's machine id
extern bool g_attached;

// ---- views on the mapping ---------------------------------------------------
//
// All of these are meaningless before attach_shared_segment has run.

inline bool pool_contains(uintptr_t a, size_t n) {
    if (g_pool == nullptr) {
        return false;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(g_pool);
    uintptr_t end = base + g_header->cxl_size;
    return a >= base && a + static_cast<uintptr_t>(n) <= end;
}

// The watched region containing [a, a+n), or kMaxWatchRegions for none. The
// slow path behind region_resolve, reached only on a cache miss.
inline uint32_t watch_index(uintptr_t a, size_t n) {
    for (uint32_t i = 0; i < g_header->watch_count; ++i) {
        const WatchRegion& w = g_header->watch[i];
        uintptr_t base = g_watch_base[i];
        if (w.in_use && base != 0 && a >= base &&
            a + static_cast<uintptr_t>(n) <= base + w.size) {
            return i;
        }
    }
    return kMaxWatchRegions;
}

// ---- resolving an address to its region -------------------------------------
//
// The pool is one region and each watched mapping is another, so an address is
// placed before its shadow index is known. One cache entry per thread holds the
// last region resolved; a hit is one range check and one addition.
//
// Thread-local, not global: the words must be read together, and a torn read
// across another thread's update would pair one region's bounds with another's
// shadow index.
extern thread_local uintptr_t t_region_lo;
extern thread_local uintptr_t t_region_hi;
extern thread_local uint64_t t_region_bias; // shadow index = addr + bias
extern thread_local uint32_t t_region_gen;

// Bumped by csan_watch, invalidating every cache entry in this process. A
// forked child inherits the parent's cached bounds and must not keep using them
// once it maps the region for itself.
extern uint32_t g_region_gen;

// Point this thread's cache at the region containing `a`. False, and the cache
// left as it was, when `a` is in none. Defined in globals.cpp.
bool region_resolve(uintptr_t a);

// Is this CXL memory the checker tracks -- its own pool, or a watched region?
inline bool cxl_contains(uintptr_t a, size_t n) {
    if (t_region_gen == g_region_gen && a >= t_region_lo &&
        a + static_cast<uintptr_t>(n) <= t_region_hi) {
        return true;
    }
    return region_resolve(a) && a + static_cast<uintptr_t>(n) <= t_region_hi;
}

// Where this address lives in the flat shadow space: the pool occupies
// [0, cxl_size), each watched region follows at its own shadow_off. Defined
// only where cxl_contains holds.
inline uint64_t shadow_offset(uintptr_t a) {
    if (t_region_gen == g_region_gen && a >= t_region_lo && a < t_region_hi) {
        return a + t_region_bias;
    }
    return region_resolve(a) ? a + t_region_bias : 0;
}

inline ByteCell* word_cell_at(uintptr_t a) {
    return &g_cells[shadow_offset(a) / kWordBytes];
}

inline LineCell* line_cell_at(uintptr_t a) {
    return &g_lines[shadow_offset(a) / kLineBytes];
}

// Hardware-coherent, and so not checked. Read without the line lock: the flag
// is set at registration, before the memory is in use, and never changes.
inline bool line_is_hwcc(const LineCell* lc) {
    return (__atomic_load_n(&lc->flags, __ATOMIC_RELAXED) & kHwCcBit) != 0;
}

inline uint32_t machine_of(uint32_t tid) {
    if (tid >= kMaxThreads) {
        return UINT32_MAX;
    }
    return atomic_load_u32(&g_header->machine_of_thread[tid]);
}


} // namespace csan

#endif
