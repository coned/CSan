// SPDX-License-Identifier: MIT
// A thread looks up a page while it is unwatched, then part of the page is
// watched, then a byte outside the watched part is read: none of these may
// hide the watched bytes from that thread's later accesses.
// Expected: race (write then read, no edge), exactly one.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <sys/mman.h>
#include <thread>

int main() {
    setenv("CSAN_WATCH_BYTES", "65536", 1);
    csan_init(1, 0, 1u << 16);
    void* m = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        std::fprintf(stderr, "mmap failed\n");
        return 1;
    }
    volatile char* p = static_cast<char*>(m);
    char v = p[64]; // unwatched: not checked
    v = static_cast<char>(v + p[0]);

    csan_watch(3, const_cast<char*>(p) + 64, 128);
    v = static_cast<char>(v + p[0]); // still outside the watched part

    std::atomic<bool> wrote{false};
    std::thread w([&] {
        p[64] = 1; // no edge to the main thread's read below
        wrote.store(true, std::memory_order_relaxed);
    });
    while (!wrote.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    v = static_cast<char>(v + p[64]);
    w.join();
    (void)v;
    return csan_expect_race(CSAN_RACE_DATA_RACE_WR).races(1).finish();
}
