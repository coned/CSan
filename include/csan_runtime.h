// SPDX-License-Identifier: MIT
/* Public interface to csan, used by programs, tests and the LLVM pass.
 *
 * One OS process per simulated machine; its threads belong to that machine.
 * All state lives in one POSIX shared-memory segment.
 *
 * Release/acquire orders only within a machine. Across machines it takes
 * write-back and publish on one side, flush and acquire on the other. */
#ifndef CSAN_RUNTIME_H
#define CSAN_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create or attach to the shared segment and bind this process to `machine`.
 *
 *   hosts         number of machines        (0 = CSAN_HOSTS, else 1)
 *   machine       this process's machine id (UINT32_MAX = CSAN_MACHINE_ID, else 0)
 *   cxl_mem_size  bytes of CXL memory       (0 = CSAN_TOTAL_MEM, else 1 MiB)
 *
 * The first process creates the segment and sets CSAN_SHM_NAME; later ones
 * attach and ignore cxl_mem_size. */
#ifdef __cplusplus
void csan_init(uint32_t hosts = 0, uint32_t machine = UINT32_MAX, uint64_t cxl_mem_size = 0);
#else
void csan_init(uint32_t hosts, uint32_t machine, uint64_t cxl_mem_size);
#endif

/* In a forked child: new thread id on `machine`, ordered after the parent.
 * Call first thing after fork(). */
void csan_spawn(uint32_t hosts, uint32_t machine);

/* After waitpid: order the caller after machine process `pid`. */
void csan_join(uint32_t pid);

/* Allocate from the CXL pool; same address in every process. NULL if full. */
void* csan_alloc(size_t size);

/* Region `id`, shared by all machines: the first caller allocates `size`
 * bytes, later callers get the same region. NULL if the pool is full or the
 * existing region is smaller than `size`. */
void* csan_shared_region(uint32_t id, size_t size);

/* Bump allocator: free untracks the range but reclaims nothing. */
void* csan_realloc(void* ptr, size_t size);
void csan_free(void* ptr);

/* Register pool memory not obtained from csan_alloc. Ranges outside the pool
 * are rejected.
 *
 *   scc   software-coherent (default): visibility is checked.
 *   hwcc  hardware-coherent: accesses are not checked, but a mutex or atomic
 *         there still carries happens-before. */
void csan_register_scc_range(void* base, size_t size);
void csan_register_hwcc_range(void* base, size_t size);
void csan_register_range(void* base, size_t size);
void csan_unregister_range(void* base);

/* Check CXL memory the checker did not allocate. [base, base+size) is this
 * process's mapping of region `id`. Every machine must use the same id and
 * size; addresses may differ. `base` must be 64-byte aligned; `size` rounds up
 * to a line. Exceeding the CSAN_WATCH_BYTES budget is refused and counted as
 * an overflow. */
void csan_watch(uint32_t id, void* base, size_t size);

/* This thread's global id, this process's machine id, findings so far. */
uint32_t csan_thread_id(void);
uint32_t csan_machine_id(void);
uint64_t csan_race_events(void);

/* ---- compatibility layers ---- */
/* Pool base (same address in every process) and size, for layers emulating
 * another pool API. NULL / 0 before init. */
void* __csan_pool_base(void);
uint64_t __csan_pool_size(void);

/* ---- hooks the pass inserts ---- */
/* Call directly only when writing a synchronization primitive. */

/* At main() entry. Attaches only if CSAN_SHM_NAME is set and CSAN_DEFER_INIT
 * is not. */
void __csan_init(void);

void __csan_register_range(void* base, size_t size);
void __csan_register_scc_range(void* base, size_t size);
void __csan_register_hwcc_range(void* base, size_t size);
void __csan_unregister_range(void* base);
int __csan_is_cxl(const void* addr, size_t n);

uint32_t __csan_thread_id(void);
void __csan_set_thread_id(uint32_t tid);
uint32_t __csan_machine_id(void);
void __csan_set_machine(uint32_t mid);
void __csan_set_hosts(uint32_t hosts);
void __csan_set_cxl_size(uint64_t size);

/* Plain accesses; non-CXL addresses are ignored. A write also resets the
 * read clock, relays and sync state of the bytes it covers. */
void __csan_read(const void* addr, size_t n);
void __csan_write(const void* addr, size_t n);
void __csan_memcpy(void* dst, const void* src, size_t n);

/* Source of a write-back or flush, for reports only. OR CSAN_SRC_ASM for
 * inline assembly. */
enum {
    CSAN_SRC_UNKNOWN = 0,
    CSAN_SRC_CLWB = 1,
    CSAN_SRC_CLFLUSHOPT = 2,
    CSAN_SRC_CLFLUSH = 3,  /* also self-fences */
    CSAN_SRC_NT_STORE = 4, /* _mm_stream_*: reaches memory without a clwb */
    CSAN_SRC_KIND_MASK = 0xFF,
    CSAN_SRC_ASM = 0x100
};

/* clwb / clflushopt over the lines of [addr, addr+n) (the pass passes n = 1).
 * A later fence completes them. `src` is a CSAN_SRC_* value. */
void __csan_wb(const void* addr, size_t n, uint32_t src);
void __csan_flush(const void* addr, size_t n, uint32_t src);

/* x86 fences; they complete publication and acquisition. */
void __csan_sfence(void);
void __csan_mfence(void);

/* atomic_thread_fence: clocks only, no write-back ordering (no x86
 * instruction). seq_cst also gets __csan_mfence. */
void __csan_fence_acquire(void);
void __csan_fence_release(void);
void __csan_fence_acqrel(void);

/* Library mutex. Call lock after acquiring, unlock before releasing. */
void __csan_lock(const void* lock);
void __csan_unlock(const void* lock);

/* Clock effect only of a release/acquire the caller performed itself. A
 * release store starts a new release sequence at `addr`; a release RMW
 * extends the existing one. */
void __csan_release(const void* addr);
void __csan_release_rmw(const void* addr);
void __csan_acquire(const void* addr);

/* Atomics: perform the access and its happens-before effect together.
 *
 *   flags  bit0 acquire, bit1 release, bit2 also do read/write bookkeeping,
 *          bit4 failure-ordering acquire (cmpxchg; bits 0-1 are then the
 *          success ordering). A failed cmpxchg never releases. */
uint64_t __csan_atomic_load(const void* addr, uint32_t size, uint32_t flags);
void __csan_atomic_store(void* addr, uint64_t val, uint32_t size, uint32_t flags);
uint64_t __csan_atomic_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t flags);
uint64_t __csan_atomic_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size,
                           uint32_t flags);

/* After pthread_join / std::thread::join: order the caller after the joined
 * thread. Creation needs no hook; pthread_create is interposed. */
void __csan_pthread_join(uint64_t pth);

const char* __csan_report(void);
uint64_t __csan_read_events(void);
uint64_t __csan_write_events(void);
uint64_t __csan_race_events(void);

/* Finding kinds; the comments are their report names. Tests depend on the
 * numbering: append, never renumber. */
enum {
    CSAN_RACE_DATA_RACE_WR = 0,              /* data-race-wr: a store, then an unordered load */
    CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE = 1,  /* read-without-byte-acquire */
    /* Was write-before-hb-line-write; stores to one line need no order among
     * themselves, so it is never reported. Kept so the numbering holds. */
    CSAN_RACE_RETIRED_2 = 2,
    CSAN_RACE_DATA_RACE_RW = 3,              /* data-race-rw: a load, then an unordered store */
    CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE = 4, /* write-without-line-acquire */
    /* write-without-line-acquire for an atomic store: another machine wrote a
     * byte of the line this store leaves, with no write-back/fence/flush/
     * fence chain between, so this store's write-back carries a stale copy
     * of it. Atomic writes to the same location are not reported. */
    CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE = 5, /* atomic-write-without-line-acquire */
    CSAN_RACE_DATA_RACE_WW = 6, /* data-race-ww: a store, then an unordered store */
    /* Write over bytes another machine wrote and never wrote back: its dirty
     * line can later overwrite this store. */
    CSAN_RACE_WRITE_WITHOUT_PUBLISH = 7, /* write-without-publish */
    CSAN_RACE_KIND_COUNT = 8
};

/* Count and name of `kind`; out of range gives 0 and "?". */
uint64_t __csan_race_kind_count(uint32_t kind);
const char* __csan_race_kind_name(uint32_t kind);

/* Records the runtime had no room for. Each can cause a false report, so this
 * must stay zero. */
uint64_t __csan_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif
