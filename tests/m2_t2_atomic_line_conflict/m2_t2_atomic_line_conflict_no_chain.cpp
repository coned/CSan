// SPDX-License-Identifier: MIT
// Two machines atomically store to different words of one line with no publish or acquire.
// Expected: race (atomic-write-without-line-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;

struct Pair {
    std::atomic<uint64_t> a; // line offset 0
    std::atomic<uint64_t> b; // line offset 8, same line
};

int writer_b(void* arg) {
    Pair* p = static_cast<Pair*>(arg);
    p->b.store(2, std::memory_order_relaxed);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    void* raw = csan_alloc(64);
    Pair* p = new (raw) Pair();
    p->a.store(0, std::memory_order_relaxed);
    p->b.store(0, std::memory_order_relaxed);

    // Machine 0's own atomic store: the line's last write, at offset 0.
    p->a.store(1, std::memory_order_relaxed);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer_b, p);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_ATOMIC_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
