// SPDX-License-Identifier: MIT
/* The public interface to csan: what a program, a test, or the LLVM pass calls.
 *
 * Each simulated machine is one OS process. A process declares its machine id
 * once -- csan_init, or csan_spawn right after fork -- and every thread it
 * creates belongs to that machine. All state lives in one POSIX shared-memory
 * segment; runtime/state/layout.h gives its layout, runtime/rules/rules.h the
 * rules each hook fires.
 *
 * Atomic orderings are tracked per machine: a release on one machine and an
 * acquire on another establish no happens-before. Crossing machines takes
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
 * The first process creates the segment and publishes its name in
 * CSAN_SHM_NAME; later processes attach and ignore cxl_mem_size. */
#ifdef __cplusplus
void csan_init(uint32_t hosts = 0, uint32_t machine = UINT32_MAX, uint64_t cxl_mem_size = 0);
#else
void csan_init(uint32_t hosts, uint32_t machine, uint64_t cxl_mem_size);
#endif

/* Join from a forked child: fresh thread id, bound to `machine`, with a
 * happens-before edge from the parent. Call it first thing after fork(). */
void csan_spawn(uint32_t hosts, uint32_t machine);

/* Join with machine process `pid` after waitpid, so the parent's later reads
 * are ordered after that machine's writes. */
void csan_join(uint32_t pid);

/* Allocate from the CXL pool. The pointer is the same virtual address in every
 * machine process. NULL when the pool is exhausted. */
void* csan_alloc(size_t size);

/* The pool region every machine knows by `id`. The first caller in any process
 * carves `size` bytes out; later callers get the same region. NULL if the pool
 * is too small, or the region exists and is smaller. */
void* csan_shared_region(uint32_t id, size_t size);

/* Reallocate / free pool memory. The allocator bumps, so free stops accounting
 * the range but reclaims nothing. */
void* csan_realloc(void* ptr, size_t size);
void csan_free(void* ptr);

/* Register pool memory handed out by hand; allocations register themselves.
 * Ranges outside the pool are rejected.
 *
 *   scc   software-coherent. The default. Cross-machine visibility is the
 *         program's job and the checker checks it.
 *   hwcc  hardware-coherent. No access to it is recorded or reported. A mutex
 *         or atomic living there still carries happens-before. */
void csan_register_scc_range(void* base, size_t size);
void csan_register_hwcc_range(void* base, size_t size);
void csan_register_range(void* base, size_t size);
void csan_unregister_range(void* base);

/* Watch CXL memory the checker did not allocate.
 *
 * [base, base+size) is this process's mapping of the region named `id`. Every
 * machine registers the same id with the same size; the addresses may differ,
 * since state is keyed by (id, offset) and never by address.
 *
 * `base` must be 64-byte aligned; `size` rounds up to a line. Shadow comes from
 * the CSAN_WATCH_BYTES budget fixed at segment creation -- exceeding it is
 * refused and counted as an overflow. */
void csan_watch(uint32_t id, void* base, size_t size);

/* This thread's global id, this process's machine id, findings so far. */
uint32_t csan_thread_id(void);
uint32_t csan_machine_id(void);
uint64_t csan_race_events(void);

/* ------------------------------------------------------------------ */
/* For a compatibility layer standing in for another system's pool API.
 * Ordinary programs use csan_alloc above.
 *
 * The pool base is the same virtual address in every machine process, so a
 * layer can hand out pool-relative offsets and convert them back. NULL / 0
 * before init. */
void* __csan_pool_base(void);
uint64_t __csan_pool_size(void);

/* ------------------------------------------------------------------ */
/* Hooks the LLVM pass inserts. Call these directly only when writing a
 * synchronization primitive. */

/* Inserted at main() entry. Attaches only when CSAN_SHM_NAME is set, and not
 * at all under CSAN_DEFER_INIT. */
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

/* Plain accesses. Addresses outside CXL memory are ignored, so a caller need
 * not check. A write also resets the read clock, the relays and the
 * synchronization state of every byte it covers. */
void __csan_read(const void* addr, size_t n);
void __csan_write(const void* addr, size_t n);
void __csan_memcpy(void* dst, const void* src, size_t n);

/* Which construct produced a write-back or flush. Reporting only; the rule is
 * the same for all of them. CSAN_SRC_ASM is OR-ed in for inline assembly. */
enum {
    CSAN_SRC_UNKNOWN = 0,
    CSAN_SRC_CLWB = 1,
    CSAN_SRC_CLFLUSHOPT = 2,
    CSAN_SRC_CLFLUSH = 3,  /* also self-fences */
    CSAN_SRC_NT_STORE = 4, /* _mm_stream_*: reaches memory without a clwb */
    CSAN_SRC_KIND_MASK = 0xFF,
    CSAN_SRC_ASM = 0x100
};

/* clwb and clflushopt over the lines spanned by [addr, addr+n). The pass passes
 * n = 1, since the instruction acts on the one line holding its operand. Each
 * records its epoch and advances the thread's clock; a following fence
 * completes it. `src` is a CSAN_SRC_* value, used only for reporting. */
void __csan_wb(const void* addr, size_t n, uint32_t src);
void __csan_flush(const void* addr, size_t n, uint32_t src);

/* x86 store and full fences. These complete publication and acquisition. */
void __csan_sfence(void);
void __csan_mfence(void);

/* C++ atomic_thread_fence. Clocks only: on x86 these emit no instruction, so
 * they order no write-back. A seq_cst fence emits mfence, and the pass calls
 * __csan_mfence for it as well. */
void __csan_fence_acquire(void);
void __csan_fence_release(void);
void __csan_fence_acqrel(void);

/* A recognized library mutex. The lock hook runs once the lock is HELD, the
 * unlock hook while it is STILL held. */
void __csan_lock(const void* lock);
void __csan_unlock(const void* lock);

/* Release and acquire on an atomic location, for call sites the pass does not
 * replace outright. A release STORE starts a release sequence at the address
 * and overwrites what was published there; a release RMW extends the existing
 * sequence and merges into it. */
void __csan_release(const void* addr);
void __csan_release_rmw(const void* addr);
void __csan_acquire(const void* addr);

/* Atomics. These perform the access and its happens-before effect together;
 * the pass replaces atomic instructions with them.
 *
 *   flags  bit0 acquire, bit1 release, bit2 also do read/write bookkeeping,
 *          bit4 the FAILURE ordering's acquire (cmpxchg only). For cmpxchg,
 *          bits 0-1 are the success ordering. A failing compare-exchange
 *          performs no write and so never releases. */
uint64_t __csan_atomic_load(const void* addr, uint32_t size, uint32_t flags);
void __csan_atomic_store(void* addr, uint64_t val, uint32_t size, uint32_t flags);
uint64_t __csan_atomic_rmw(void* addr, uint64_t val, uint32_t size, uint32_t op, uint32_t flags);
uint64_t __csan_atomic_cas(void* addr, uint64_t expected, uint64_t desired, uint32_t size,
                           uint32_t flags);

/* pthread_join and std::thread::join: merges the joined thread's clock into the
 * caller's. Thread creation has no hook -- the runtime interposes
 * pthread_create, which libstdc++ also creates threads through. */
void __csan_pthread_join(uint64_t pth);

const char* __csan_report(void);
uint64_t __csan_read_events(void);
uint64_t __csan_write_events(void);
uint64_t __csan_race_events(void);

/* The kinds of finding, one per report site. Tests assert on these, so the
 * numbering is interface: append, never renumber. The comments are the strings
 * that appear in a report. */
enum {
    CSAN_RACE_READ_BEFORE_HB_WRITE = 0,       /* read-before-hb-write */
    CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE = 1,  /* read-without-byte-acquire */
    CSAN_RACE_WRITE_BEFORE_HB_LINE_WRITE = 2, /* write-before-hb-line-write */
    CSAN_RACE_WRITE_AFTER_READ = 3,           /* write-after-read */
    CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE = 4, /* write-without-line-acquire */
    /* An atomic write to a line another machine wrote at a DIFFERENT location,
     * with no write-back/fence/flush/fence chain between them: each machine's
     * write-back carries its own stale copy of the other's bytes. Two atomic
     * writes to the SAME location are not reported. A plain write in this
     * position is CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE. */
    CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE = 5, /* atomic-write-without-line-acquire */
    CSAN_RACE_DATA_RACE = 6, /* data-race */
    CSAN_RACE_KIND_COUNT = 7
};

/* Findings of `kind` so far, and its name. Out-of-range kinds count 0, name "?". */
uint64_t __csan_race_kind_count(uint32_t kind);
const char* __csan_race_kind_name(uint32_t kind);

/* Records the runtime could not store: a publication or acquisition slot with
 * no room, or a failed arena allocation. A dropped record turns a satisfied
 * visibility query into a false report, so this must stay zero. Printed in the
 * stats line at exit. */
uint64_t __csan_overflow_count(void);

#ifdef __cplusplus
}
#endif

#endif
