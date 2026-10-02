// SPDX-License-Identifier: MIT
// Shared helper for the scenario tests: forks machine processes, repeats each
// scenario, and turns the runtime's counters into a CTest exit code.
#pragma once

#include "csan_runtime.h"

#include <cinttypes>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>

// Wall-clock guard: a hung hook fails the test instead of hanging the suite.
#ifndef CSAN_TEST_TIMEOUT_SECONDS
#define CSAN_TEST_TIMEOUT_SECONDS 60
#endif

extern "C" inline void csan_test_timeout(int) {
    static const char msg[] = "FAIL csan_test: timed out; a hook is stuck (deadlock?)\n";
    // write, not fprintf: this runs in a signal handler.
    ssize_t n = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)n;
    _exit(124);
}

// alarm() does not survive fork, so every process arms its own.
static inline void csan_test_arm_timeout() {
    struct sigaction sa;
    sa.sa_handler = csan_test_timeout;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: a blocked waitpid must return, not resume
    sigaction(SIGALRM, &sa, nullptr);
    alarm(CSAN_TEST_TIMEOUT_SECONDS);
}

// Every check also requires that the checker observed the program
// (read + write events > 0) unless .allow_no_events() is given.
//   .kind(K) / .no_kind(K)        finding K fired / did not fire
//   .races(n) .reads(n) .writes(n) exact counts; .min_reads/.min_writes bounds
//   .any_verdict()                skip the race/no-race check (schedule-dependent)
// Counts are totals over every machine process (they live in the shared segment).
//   return csan_expect_race(CSAN_RACE_WRITE_AFTER_READ).races(1).finish();
class CsanExpect {
public:
    explicit CsanExpect(bool expect_race) : expect_race_(expect_race) {}

    CsanExpect& kind(uint32_t k) { return add(require_, k); }
    CsanExpect& no_kind(uint32_t k) { return add(forbid_, k); }
    CsanExpect& races(uint64_t n) { races_ = static_cast<int64_t>(n); return *this; }
    CsanExpect& reads(uint64_t n) { reads_ = static_cast<int64_t>(n); return *this; }
    CsanExpect& writes(uint64_t n) { writes_ = static_cast<int64_t>(n); return *this; }
    CsanExpect& min_reads(uint64_t n) { min_reads_ = static_cast<int64_t>(n); return *this; }
    CsanExpect& min_writes(uint64_t n) { min_writes_ = static_cast<int64_t>(n); return *this; }
    CsanExpect& allow_no_events() { need_events_ = false; return *this; }
    CsanExpect& any_verdict() { need_verdict_ = false; return *this; }

    int finish() const {
        uint64_t rd = __csan_read_events();
        uint64_t wr = __csan_write_events();
        uint64_t races_seen = csan_race_events();

        // A dropped shadow record can cause a false report.
        uint64_t dropped = __csan_overflow_count();
        if (dropped != 0) {
            return fail(rd, wr, races_seen, "%" PRIu64 " shadow records were dropped (overflow)",
                        dropped);
        }
        if (need_events_ && rd + wr == 0) {
            return fail(rd, wr, races_seen,
                        "the checker observed nothing: no reads and no writes reached it, so this "
                        "scenario proves nothing (is the pass plugin in the compile line?)");
        }
        if (need_verdict_ && (races_seen > 0) != expect_race_) {
            return fail(rd, wr, races_seen, "expected %s", expect_race_ ? "a race" : "no race");
        }
        if (races_ >= 0 && races_seen != static_cast<uint64_t>(races_)) {
            return fail(rd, wr, races_seen, "expected exactly %" PRId64 " findings", races_);
        }
        for (int i = 0; i < n_require_; ++i) {
            if (__csan_race_kind_count(require_[i]) == 0) {
                return fail(rd, wr, races_seen, "expected a %s finding, and none was reported",
                            __csan_race_kind_name(require_[i]));
            }
        }
        for (int i = 0; i < n_forbid_; ++i) {
            uint64_t n = __csan_race_kind_count(forbid_[i]);
            if (n != 0) {
                return fail(rd, wr, races_seen, "expected no %s finding, and %" PRIu64 " were reported",
                            __csan_race_kind_name(forbid_[i]), n);
            }
        }
        if (reads_ >= 0 && rd != static_cast<uint64_t>(reads_)) {
            return fail(rd, wr, races_seen, "expected exactly %" PRId64 " reads", reads_);
        }
        if (writes_ >= 0 && wr != static_cast<uint64_t>(writes_)) {
            return fail(rd, wr, races_seen, "expected exactly %" PRId64 " writes", writes_);
        }
        if (min_reads_ >= 0 && rd < static_cast<uint64_t>(min_reads_)) {
            return fail(rd, wr, races_seen, "expected at least %" PRId64 " reads", min_reads_);
        }
        if (min_writes_ >= 0 && wr < static_cast<uint64_t>(min_writes_)) {
            return fail(rd, wr, races_seen, "expected at least %" PRId64 " writes", min_writes_);
        }

        std::printf("PASS expect_race=%s races=%" PRIu64 " reads=%" PRIu64 " writes=%" PRIu64 "\n",
                    need_verdict_ ? (expect_race_ ? "1" : "0") : "any", races_seen, rd, wr);
        return 0;
    }

private:
    static constexpr int kMaxKinds = 4;

    CsanExpect& add(uint32_t* list, uint32_t k) {
        int& n = (list == require_) ? n_require_ : n_forbid_;
        if (n < kMaxKinds) {
            list[n++] = k;
        }
        return *this;
    }

    int fail(uint64_t rd, uint64_t wr, uint64_t races_seen, const char* fmt, ...) const
        __attribute__((format(printf, 5, 6))) {
        std::fprintf(stderr, "FAIL expect_race=%s races=%" PRIu64 " reads=%" PRIu64
                             " writes=%" PRIu64 ": ",
                     need_verdict_ ? (expect_race_ ? "1" : "0") : "any", races_seen, rd, wr);
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        va_end(ap);
        std::fputc('\n', stderr);
        for (uint32_t k = 0; k < CSAN_RACE_KIND_COUNT; ++k) {
            uint64_t n = __csan_race_kind_count(k);
            if (n != 0) {
                std::fprintf(stderr, "     reported: %-28s %" PRIu64 "\n",
                             __csan_race_kind_name(k), n);
            }
        }
        return 1;
    }

    bool expect_race_;
    bool need_events_ = true;
    bool need_verdict_ = true;
    int64_t races_ = -1, reads_ = -1, writes_ = -1, min_reads_ = -1, min_writes_ = -1;
    uint32_t require_[kMaxKinds] = {};
    uint32_t forbid_[kMaxKinds] = {};
    int n_require_ = 0, n_forbid_ = 0;
};

// Value check: the runtime performs every atomic itself, so a wrong opcode
// mapping would corrupt data while leaving the verdict correct.
static inline bool csan_test_eq(const char* what, uint64_t got, uint64_t want) {
    if (got == want) {
        return true;
    }
    std::fprintf(stderr, "FAIL %s: got %" PRIu64 " (0x%" PRIx64 "), want %" PRIu64 " (0x%" PRIx64 ")\n",
                 what, got, got, want, want);
    return false;
}

static inline CsanExpect csan_expect_no_race() { return CsanExpect(false); }
static inline CsanExpect csan_expect_race(uint32_t kind) { return CsanExpect(true).kind(kind); }
static inline CsanExpect csan_expect_any_race() { return CsanExpect(true); }
static inline CsanExpect csan_expect_any_verdict() { return CsanExpect(false).any_verdict(); }

static inline int csan_test_finish(bool expect_race) {
    return CsanExpect(expect_race).finish();
}

// Run `role` in a forked child bound to `machine`; pool pointers stay valid.
static inline pid_t csan_test_spawn_machine(uint32_t hosts, uint32_t machine,
                                           int (*role)(void*), void* arg) {
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("csan_test: fork");
        return -1;
    }
    if (pid == 0) {
        csan_test_arm_timeout();
        csan_spawn(hosts, machine);
        int rc = role(arg);
        fflush(nullptr);
        _exit(rc);
    }
    return pid;
}

// Wait for a machine child (a join edge) and return its exit code.
static inline int csan_test_wait(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::perror("csan_test: waitpid");
        return -1;
    }
    csan_join(static_cast<uint32_t>(pid));
    if (!WIFEXITED(status)) {
        std::fprintf(stderr, "csan_test: child did not exit normally\n");
        return -1;
    }
    return WEXITSTATUS(status);
}

// Each scenario's main runs CSAN_TEST_REPEATS times, one forked child per
// repetition with its own pid-named segment; the first failure ends the test.
#ifndef CSAN_TEST_REPEATS
#define CSAN_TEST_REPEATS 1
#endif
#if CSAN_TEST_REPEATS < 1
#error "CSAN_TEST_REPEATS must be at least 1"
#endif

int scenario_body();

static inline void csan_test_replay(FILE* captured) {
    if (captured == nullptr) {
        return;
    }
    std::rewind(captured);
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), captured)) > 0) {
        std::fwrite(buf, 1, n, stderr);
    }
    std::fflush(stderr);
}

int main() {
    csan_test_arm_timeout();
    int repeats = CSAN_TEST_REPEATS;

    // A named segment is shared, so repetitions could not start clean.
    const char* shm = std::getenv("CSAN_SHM_NAME");
    if (repeats > 1 && shm != nullptr && *shm != '\0') {
        std::fprintf(stderr, "csan_test: CSAN_SHM_NAME is set, running one repetition\n");
        repeats = 1;
    }
    if (repeats == 1) {
        return scenario_body();
    }

    for (int i = 1; i <= repeats; ++i) {
        // Earlier repetitions are captured and shown only if they fail.
        FILE* captured = (i < repeats) ? std::tmpfile() : nullptr;
        std::fflush(nullptr);

        pid_t pid = fork();
        if (pid < 0) {
            std::perror("csan_test: fork");
            if (captured != nullptr) {
                std::fclose(captured);
            }
            return 1;
        }
        if (pid == 0) {
            csan_test_arm_timeout();
            if (captured != nullptr) {
                int fd = fileno(captured);
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
            }
            int rc = scenario_body();
            // exit, not _exit: the runtime's atexit hook unlinks the segment.
            std::exit(rc);
        }

        csan_test_arm_timeout(); // a fresh budget per repetition
        int rc = csan_test_wait(pid);
        alarm(0);
        if (rc != 0) {
            csan_test_replay(captured);
            std::fprintf(stderr, "FAIL repetition %d of %d\n", i, repeats);
            if (captured != nullptr) {
                std::fclose(captured);
            }
            return rc > 0 ? rc : 1;
        }
        if (captured != nullptr) {
            std::fclose(captured);
        }
    }

    std::printf("PASS %d/%d repetitions\n", repeats, repeats);
    return 0;
}

// Must come last. The name must not start with csan_: the pass skips those.
#define main scenario_body
