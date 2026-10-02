// SPDX-License-Identifier: MIT
// Two machines atomically store to the same atomic variable.
// Expected: no race (same bytes cannot carry stale data).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <new>

namespace {
constexpr uint32_t kHosts = 2;

int writer(void* arg) {
    auto* a = static_cast<std::atomic<uint64_t>*>(arg);
    a->store(2, std::memory_order_relaxed);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    auto* a = new (csan_alloc(64)) std::atomic<uint64_t>(0);

    a->store(1, std::memory_order_relaxed);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &writer, a);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_no_race().finish();
}
