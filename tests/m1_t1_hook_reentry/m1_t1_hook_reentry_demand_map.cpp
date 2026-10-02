// SPDX-License-Identifier: MIT
// An atomic store faults inside the runtime on a PROT_NONE watched page; the SIGSEGV handler maps it
// and does an instrumented store. Expected: no race, no hang, and all three stores observed.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRegionId = 3;
constexpr size_t kRegionBytes = 4096;
constexpr uint32_t kStored = 0xc0ffeeu;

char* g_page = nullptr;              // the PROT_NONE page the store faults on
char* g_touched = nullptr;           // pool byte the handler writes
volatile sig_atomic_t g_faults = 0;

// Runs on the faulting thread with its hook half-finished.
void on_fault(int, siginfo_t*, void*) {
    ++g_faults;
    if (mprotect(g_page, kRegionBytes, PROT_READ | PROT_WRITE) != 0) {
        _exit(1);
    }
    *g_touched = 7;
}

} // namespace

int main() {
    // Shadow for watched regions is budgeted when the segment is created.
    setenv("CSAN_WATCH_BYTES", "65536", 1);
    csan_init(1, 0, 1u << 16);

    auto* flag = static_cast<std::atomic<uint32_t>*>(csan_alloc(sizeof(std::atomic<uint32_t>)));
    g_touched = static_cast<char*>(csan_alloc(64));
    if (flag == nullptr || g_touched == nullptr) {
        std::fprintf(stderr, "could not allocate from the pool\n");
        return 1;
    }
    // A live release makes plain writes take the global lock.
    flag->store(1, std::memory_order_release);

    void* p = mmap(nullptr, kRegionBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        std::perror("mmap");
        return 1;
    }
    g_page = static_cast<char*>(p);
    csan_watch(kRegionId, p, kRegionBytes);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &sa, nullptr) != 0) {
        std::perror("sigaction");
        return 1;
    }

    auto* target = reinterpret_cast<std::atomic<uint32_t>*>(g_page);
    target->store(kStored, std::memory_order_release); // faults inside the hook

    if (g_faults != 1) {
        std::fprintf(stderr, "FAIL the page was faulted on %d times, wanted 1\n",
                     static_cast<int>(g_faults));
        return 1;
    }
    if (!csan_test_eq("the atomic store's value", target->load(std::memory_order_relaxed),
                      kStored)) {
        return 1;
    }
    if (!csan_test_eq("the handler's store", static_cast<uint64_t>(*g_touched), 7)) {
        return 1;
    }
    // Writes: 4 + 4 atomic, 1 by the handler. Reads: 4 + 1 by the checks.
    return csan_expect_no_race().writes(9).reads(5).finish();
}
