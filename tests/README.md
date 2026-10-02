# tests

One directory per race scenario, one `.cpp` file per variant. CMake compiles
each file with clang and the pass plugin, links the runtime, and registers it
with CTest. Each file states its own expectation and returns nonzero when it
does not hold, so the test is the binary.

## Run

```bash
ctest --test-dir build --output-on-failure      # all
ctest --test-dir build -R m2_t2_rw_conflict     # one family
ctest --test-dir build -R _missing_publish      # one variant across families
```

Every test runs its scenario `CSAN_TEST_REPEATS` times (200 by default), each
repetition in a fresh child process. The first wrong verdict ends the test and
prints `FAIL repetition 7 of 200`; a clean one ends with `PASS 200/200
repetitions`. Reconfigure with `-DCSAN_TEST_REPEATS=1` while iterating.

To see where a finding came from, build with one repetition and run the binary
directly with tracing on:

```bash
cmake -S . -B build -DCSAN_TEST_REPEATS=1 && cmake --build build -j 8
CSAN_TRACE=1 ./build/m2_t2_rw_conflict_missing_publish
```

Tracing prints a call-stack block per finding, so 200 repetitions of a race
scenario is not worth waiting for.

## Naming

`m<machines>_t<threads>_<pattern>/` — `m2_t3_rw_third_thread_flush` is two
machines and three threads. Variants inside share that prefix:

| Suffix | Meaning |
| --- | --- |
| `_no_race` | the minimal correct protocol |
| `_missing_<step>` | that protocol with one step deleted |
| anything else | names the broken step |

`ls tests/` is the list of scenarios; each one's header comment says what it
does and what it expects.

## Writing one

```cpp
#include "csan_runtime.h"
#include "csan_test.h"
#include <immintrin.h>

struct Ctx { char* x; };

static int reader(void* arg) {          // runs as machine 1, in a child process
    Ctx* c = static_cast<Ctx*>(arg);
    _mm_clflushopt(c->x);               // flush
    _mm_mfence();                       // acquire
    volatile char v = c->x[0];           // read
    (void)v;
    return 0;
}

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.x[0] = 42;                       // write
    _mm_clwb(ctx.x);                     // write back
    // missing: _mm_sfence();            // publish

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
```

Drop it in `tests/<scenario>/` and reconfigure. No registration step.

Header comment first, saying what the scenario does and what it expects.

### Expectations

| Call | Asserts |
| --- | --- |
| `csan_expect_no_race()` | no finding |
| `csan_expect_race(K)` | at least one finding of kind `K` (`CSAN_RACE_*`) |
| `csan_expect_any_race()` | at least one finding, any kind |
| `.kind(K)` / `.no_kind(K)` | `K` did / did not fire |
| `.races(n)` | exactly `n` findings |
| `.reads(n)` / `.writes(n)` | exact event counts |
| `.min_reads(n)` / `.min_writes(n)` | lower bounds, where the count depends on the schedule |
| `.allow_no_events()` | skip the "was anything observed" check |
| `.any_verdict()` | skip the race / no-race check |
| `.finish()` | run the checks and return the exit code |

Every expectation also requires that the checker observed at least one access
and that nothing overflowed, so a scenario built without the pass fails rather
than passing with `races=0`.

`csan_test_eq(what, got, want)` asserts a program value instead of a verdict —
the pass replaces every atomic with a runtime call, so a scenario exercising
one should also say what value it must produce.

### Helpers

| Call | Does |
| --- | --- |
| `csan_test_spawn_machine(hosts, machine, fn, arg)` | fork a machine and run `fn(arg)` in it |
| `csan_test_wait(pid)` | wait for it, returning its exit code |

Handshakes between machines must live in CXL memory. Static globals are
copy-on-write after `fork` and are not shared.

## Files

```text
csan_test.h              the expectation API and the repetition harness
<scenario>/*.cpp         one file per variant
ir/pass_smoke.ll         input for the pass's own test
ir/run_pass_smoke.cmake  runs the plugin with opt and requires every hook family
```

`pass_inserts_hooks` reads the instrumented IR rather than a verdict: a pass
that stopped instrumenting would turn the race scenarios red and leave every
no-race scenario green.
