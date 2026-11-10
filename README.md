# CSan

CSan is a dynamic checker for software-managed coherence on multi-host CXL
shared memory. It is an LLVM pass plus a runtime library.

On CXL memory without hardware cache coherence, a write is visible to another
machine only if:

- the writer wrote the line back (`clwb` or `clflushopt`) and fenced (`sfence` or `mfence`): it **published** the line;
- the reader flushed the line (`clflushopt`) and fenced (`mfence`): it **acquired** the line;
- the publish happens-before the acquire (through atomics, mutexes, fork and join).

CSan reports every access where this did not happen:

- **missing write-back** (no publish before the reader),
- **missing invalidate** (no acquire on the reader side),
- **atomic line conflicts** (atomics sharing a line with other machines' writes),
- **data races** (no happens-before at all).

CSan simulates several machines on one host. Each thread belongs to one machine
id. A machine can be an OS process, whose threads all belong to it, or a group
of threads inside one process. All checker state lives in one POSIX
shared-memory segment, so every process and thread sees the same state.

## Requirements

- Linux, x86-64.
- clang and LLVM. We recommend LLVM 23, with its `llvm-symbolizer` for call
  stacks in reports. The pass must be built against the same LLVM that the
  clang loads it into.

## Build

```bash
cmake -S . -B build -DLLVM_DIR=~/opt/llvm-23/lib/cmake/llvm
cmake --build build -j 8
```

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

CSan reports the read:

```text
CSAN RACE: read-without-byte-acquire at pool+0x000000 (0x78a4a3eff0c0) reader thread 1 machine 1 writer thread 0 machine 0 wb_clock=1 reader_vc_writer=1
```

The fix writes back on machine 0 and invalidates on machine 1:

```cpp
void machine0() { *x = 42; _mm_clwb((void*)x); _mm_sfence(); }
int machine1() { _mm_clflushopt((void*)x); _mm_mfence(); return *x; }
```

Each process prints its own `CSAN STATS` line at exit.

Machines as processes: start another machine with `fork()` and call
`csan_spawn(hosts, machine)` first thing in the child, or start separate
processes with `CSAN_MACHINE_ID`, `CSAN_HOSTS` and a shared `CSAN_SHM_NAME`.

Machines as threads: in one process, call `__csan_set_machine(machine)` at the
start of each thread, before it touches CXL memory. The threads it creates
afterwards belong to the same machine.

If your program maps its own shared memory, watch it instead of using the pool.
Every machine calls `csan_watch` with the same `id` and size; each may map it at
a different address:

```cpp
// After your program has its CXL memory, wherever it came from:
csan_watch(region_id, base, size_in_bytes);
```

Set `CSAN_WATCH_BYTES` to at least the total watched size.

## Reports

Report kinds: `data-race-wr`, `read-without-byte-acquire`,
`data-race-rw`, `write-without-line-acquire`,
`atomic-write-without-line-acquire`, `data-race-ww`, `write-without-publish`.

A store is checked byte by byte. It needs the bytes it overwrites to have
been written back by any other machine that wrote them (else
`write-without-publish`), and the bytes it leaves on the line to have been
flushed and fenced on this machine after another machine wrote them (else
`write-without-line-acquire`). Stores to one line need no order among
themselves. A read needs an `mfence` to have finished its flush; a store
needs only an `sfence`.

By default each finding is one line:

```text
CSAN RACE: read-without-byte-acquire at pool+0x000000 (0x78a4a3eff0c0) reader thread 1 machine 1 writer thread 0 machine 0 wb_clock=1 reader_vc_writer=1
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

Sizes accept a `K`, `M` or `G` suffix.

### Reporting

| Variable | Effect | Default |
| --- | --- | --- |
| `CSAN_TRACE=1` | Print findings with call stacks. | off |
| `CSAN_TRACE_DEPTH=n` | Events remembered per thread (at least 64). | 4096 |
| `CSAN_TRACE_STACKS=n` | Distinct call stacks kept, rounded up to a power of two (at least 256). | 65536 |
| `CSAN_SYMBOLIZER=path` | Symbolizer to run. | `llvm-symbolizer-<major>`, then `llvm-symbolizer` |
| `CSAN_TRACE_RANGE=lo:hi` | Log every event in a range of pool offsets. | off |
| `CSAN_TRACE_DEBUG` | If set, print the symbolizer's raw output. | unset |
| `CSAN_REPORT_LIMIT=n` | Findings printed before going quiet; later ones are still counted. `0` means no limit; `off` prints nothing and takes no lock. | 100 |
| `CSAN_REPORT_ONCE=0` | Check and count every finding at a byte. When on, a byte's checks are skipped after its first finding; its state is still updated. | on |
| `CSAN_DEBUG_ALLOC` | If set, print every pool allocation. | unset |

### Memory and sizing

| Variable | Effect | Default |
| --- | --- | --- |
| `CSAN_TOTAL_MEM=n` | Size of the CXL pool. | 1 MiB |
| `CSAN_WATCH_BYTES=n` | Shadow space for `csan_watch` regions; at least their total size. | 0 |
| `CSAN_THREADS=n` | Width of every vector clock, 4 to 256. | 64 |
| `CSAN_CLOCK_ENTRIES=n` | Capacity of the lock and release tables, 64 to 4194304. | 4096 |
| `CSAN_ARENA=n` | Arena bytes for variable-size shadow state (at least 1 MiB). | half the pool, at least 4 MiB |

### Machines

| Variable | Effect | Default |
| --- | --- | --- |
| `CSAN_MACHINE_ID=n` | This process's machine id. | 0 |
| `CSAN_HOSTS=n` | Number of machines, up to 64. | 1 |
| `CSAN_SHM_NAME=name` | Name of the shared segment; processes with the same name join one session. When set, a process attaches at startup. | a private `/csan_rt_<pid>` |
| `CSAN_DEFER_INIT=1` | Do not attach at startup; the program calls `csan_init` itself. | off |

### Instrumentation (read by the pass at compile time)

| Variable | Effect | Default |
| --- | --- | --- |
| `CSAN_SCOPE=sub,str` | Instrument only functions whose name contains one of these strings. | empty: instrument everything |

## API

The API is in [include/csan_runtime.h](include/csan_runtime.h).

## License

MIT. See [LICENSE](LICENSE).
