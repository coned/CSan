# csan

csan is a dynamic checker for software-managed coherence on multi-host CXL
shared memory. It is an LLVM pass plus a runtime library.

On CXL memory without hardware cache coherence, a write is visible to another
machine only if:

- the writer wrote the line back (`clwb` or `clflushopt`) and fenced (`sfence` or `mfence`): it **published** the line;
- the reader flushed the line (`clflushopt`) and fenced (`mfence`): it **acquired** the line;
- the write happens-before the read (atomics, mutexes, fork and join).

csan reports every access where this did not happen:

- **missing write-back** (no publish before the reader),
- **missing invalidate** (no acquire on the reader side),
- **atomic line conflicts** (atomics sharing a line with other machines' writes),
- **data races** (no happens-before at all).

To run without CXL memory: A **machine is an OS process**. A process binds to one machine id; every thread
it creates belongs to that machine. All checker state lives in one POSIX
shared-memory segment, so all machine processes see the same state.

## Requirements

- Linux, x86-64.
- clang and LLVM, version 15 through 23. The pass must be built against the
  same LLVM that the clang loads it into.
- `llvm-symbolizer` of the same version, for call stacks in reports.

CMake looks for `clang-<N>` and `clang++-<N>` on `PATH` (versioned names only).

## Build

```bash
cmake -S . -B build -DLLVM_DIR=~/opt/llvm-23/lib/cmake/llvm
cmake --build build -j 8
```

For another LLVM, set both version flags to match:

```bash
cmake -S . -B build-15 -DCSAN_CLANG_VERSION=15 -DLLVM_DIR=/usr/lib/llvm-15/lib/cmake/llvm
```

| CMake flag | Meaning | Default |
| --- | --- | --- |
| `LLVM_DIR` | The LLVM CMake config to build the pass against | none |
| `CSAN_CLANG_VERSION` | Major version of the clang that compiles the tests | 23 |
| `CSAN_TEST_REPEATS` | How many times each scenario runs inside one test | 200 |

## Run the tests

```bash
ctest --test-dir build --output-on-failure      # all
ctest --test-dir build -R m2_t2_rw_conflict     # one family
```

Each `tests/<scenario>/*.cpp` is one test. It states its own expected verdict
and fails when the checker disagrees. Scenarios are named
`m<machines>_t<threads>_<pattern>`. See [tests/README.md](tests/README.md).

## Check your own program

Compile with the pass plugin and link the runtime:

```bash
clang++-23 -std=c++17 -O2 -g -fno-omit-frame-pointer \
  -I include -fpass-plugin=$PWD/build/libCSanPass.so \
  -mclflushopt -mclwb \
  race.cpp build/libcsan_runtime.a -lpthread -o race
./race
```

`-g -fno-omit-frame-pointer` give readable call stacks in reports.

Two machines share one byte. Machine 0 writes it and never writes it back:

```cpp
#include "csan_runtime.h"
#include <immintrin.h>
#include <sys/wait.h>
#include <unistd.h>

volatile char* x;

void machine0() { *x = 42; }         // writes, never writes back
int machine1() { return *x; }        // reads: reported

int main() {
    csan_init(2, 0, 1 << 20);        // 2 machines; this process is machine 0
    x = (volatile char*)csan_alloc(64);
    machine0();
    if (fork() == 0) { csan_spawn(2, 1); return machine1(); }
    wait(nullptr);
}
```

csan reports the read:

```text
CSAN RACE: read-without-byte-acquire at pool+0x000000 (0x7555824ff100) reader thread 1 machine 1 writer thread 0 machine 0 wb_clock=1 reader_vc_writer=1
```

The fix writes back on machine 0 and invalidates on machine 1:

```cpp
void machine0() { *x = 42; _mm_clwb((void*)x); _mm_sfence(); }
int machine1() { _mm_clflushopt((void*)x); _mm_mfence(); return *x; }
```

Each process prints its own `CSAN STATS` line at exit.

Machines are processes. Start another machine with `fork()` and call
`csan_spawn(hosts, machine)` first thing in the child. Or start separate
processes with `CSAN_MACHINE_ID`, `CSAN_HOSTS` and a shared `CSAN_SHM_NAME`.

If your program maps its own shared memory, watch it instead of using the pool.
Every machine calls `csan_watch` with the same `id` and size; each may map it at
a different address:

```cpp
// After your program has its CXL memory, wherever it came from:
csan_watch(region_id, base, size_in_bytes);
```

Set `CSAN_WATCH_BYTES` to at least the total watched size.

## Reports

Report kinds: `read-before-hb-write`, `read-without-byte-acquire`,
`write-before-hb-line-write`, `write-after-read`, `write-without-line-acquire`,
`atomic-write-without-line-acquire`.

By default each finding is one line:

```text
CSAN RACE: read-without-byte-acquire at pool+0x000000 (0x7f2c...) reader thread 1 machine 1 writer thread 0 machine 0
```

With `CSAN_TRACE=1`, each finding is a block with call stacks: the access, the
last write it was compared against, and every publish and acquire the rule
considered, each with the code that did it.

Every run ends with a stats line:

```text
CSAN STATS: reads=1 writes=1 races=1 overflow=0 ...
```

`overflow`, `clock_table_full` and `arena_full` must be zero. A nonzero value
means state was dropped, so findings may be wrong. Raise the matching size flag.

## Environment flags

### Reporting

| Variable | Effect |
| --- | --- |
| `CSAN_TRACE=1` | Print findings with call stacks. |
| `CSAN_TRACE_DEPTH=n` | Events remembered per thread. Default 4096. |
| `CSAN_TRACE_STACKS=n` | Distinct call stacks kept, rounded to a power of two. Default 65536. |
| `CSAN_SYMBOLIZER=path` | Symbolizer to run. Default `llvm-symbolizer-<major>`, then `llvm-symbolizer`. |
| `CSAN_TRACE_RANGE=lo:hi` | Log every event in a range of pool offsets. |
| `CSAN_TRACE_DEBUG=1` | Print the symbolizer's raw output. |
| `CSAN_REPORT_LIMIT=n` | Findings printed before going quiet. Default 100; 0 means no limit. Later findings are still counted. |
| `CSAN_DEBUG_ALLOC=1` | Print every pool allocation. |

### Memory and sizing

| Variable | Effect |
| --- | --- |
| `CSAN_TOTAL_MEM=n` | Size of the CXL pool. Accepts `K`/`M`/`G`. Default 1 MiB. |
| `CSAN_WATCH_BYTES=n` | Shadow space for `csan_watch` regions. Default 0. |
| `CSAN_THREADS=n` | Width of every vector clock. Default 64, maximum 256. |
| `CSAN_CLOCK_ENTRIES=n` | Capacity of the lock and release tables. Default 4096. |
| `CSAN_ARENA=n` | Arena bytes for variable-size shadow state. Default half the pool. |

### Processes

| Variable | Effect |
| --- | --- |
| `CSAN_MACHINE_ID=n` | This process's machine id. |
| `CSAN_HOSTS=n` | Number of machines. |
| `CSAN_SHM_NAME=name` | Name of the shared segment; processes with the same name join one session. |
| `CSAN_DEFER_INIT=1` | Do not attach at startup; the program calls `csan_init` itself. |

### Instrumentation (read by the pass at compile time)

| Variable | Effect |
| --- | --- |
| `CSAN_SCOPE=sub,str` | Instrument only functions whose name contains one of these strings. Empty instruments everything. |

## API

From `include/csan_runtime.h`:

| Call | Meaning |
| --- | --- |
| `csan_init(hosts, machine, cxl_mem_size)` | Create or attach to the shared segment and bind this process to `machine`. Zero arguments fall back to `CSAN_HOSTS`, `CSAN_MACHINE_ID`, `CSAN_TOTAL_MEM`. |
| `csan_spawn(hosts, machine)` | Call first thing after `fork()`. Binds the child to `machine` and records the fork as a happens-before edge. |
| `csan_join(pid)` | After `waitpid`, record a join edge from that machine process. |
| `csan_alloc(size)` | Allocate from the CXL pool. The pointer is valid in every machine process. |
| `csan_realloc(ptr, size)` / `csan_free(ptr)` | Reallocate / free pool memory. |
| `csan_shared_region(id, size)` | The region every machine knows by `id`, carved from the pool on first call. |
| `csan_watch(id, base, size)` | Check memory the checker did not allocate. Keyed by `(id, offset)`, so each machine may map it anywhere. |
| `csan_register_range(base, size)` | Register pool memory handed out by hand. |
| `csan_register_scc_range(base, size)` | Mark a range software-coherent (the default). |
| `csan_register_hwcc_range(base, size)` | Mark a range hardware-coherent: its accesses are not checked, but synchronization in it still counts. |
| `csan_unregister_range(base)` | Undo a registration. |
| `csan_thread_id()` / `csan_machine_id()` | This thread's global id / this process's machine id. |
| `csan_race_events()` | Findings so far, across all machines. |

## Known limits

- Thread clocks are 24 bits wide and can overflow on very long runs.
- Very large watched regions (around 512 MiB) are not supported yet.

## License

MIT. See [LICENSE](LICENSE).
