// SPDX-License-Identifier: MIT
// Checks the value every forwarded atomic RMW/CAS returns and leaves in memory, at two widths.
// Expected: no race, and every value correct.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>

namespace {

bool ok = true;

void check(const char* what, uint64_t got, uint64_t want) {
    ok = csan_test_eq(what, got, want) && ok;
}

} // namespace

int main() {
    csan_init(1, 0, 1u << 16);

    auto* a = static_cast<std::atomic<uint64_t>*>(csan_alloc(sizeof(std::atomic<uint64_t>)));
    auto* w = static_cast<std::atomic<uint32_t>*>(csan_alloc(sizeof(std::atomic<uint32_t>)));
    if (a == nullptr || w == nullptr) {
        std::fprintf(stderr, "alloc failed\n");
        return 1;
    }

    // --- the six operations the pass forwards, 64-bit ------------------------

    a->store(100, std::memory_order_relaxed);
    check("fetch_add returns the old value", a->fetch_add(5, std::memory_order_relaxed), 100);
    check("fetch_add leaves the sum", a->load(std::memory_order_relaxed), 105);

    check("fetch_sub returns the old value", a->fetch_sub(5, std::memory_order_relaxed), 105);
    check("fetch_sub leaves the difference", a->load(std::memory_order_relaxed), 100);

    a->store(0xF0F0F0F0F0F0F0F0ull, std::memory_order_relaxed);
    check("fetch_and returns the old value",
          a->fetch_and(0xFF00FF00FF00FF00ull, std::memory_order_relaxed), 0xF0F0F0F0F0F0F0F0ull);
    check("fetch_and leaves the conjunction", a->load(std::memory_order_relaxed),
          0xF000F000F000F000ull);

    check("fetch_or returns the old value",
          a->fetch_or(0x0000FFFF0000FFFFull, std::memory_order_relaxed), 0xF000F000F000F000ull);
    check("fetch_or leaves the disjunction", a->load(std::memory_order_relaxed),
          0xF000FFFFF000FFFFull);

    check("fetch_xor returns the old value",
          a->fetch_xor(0xFFFFFFFFFFFFFFFFull, std::memory_order_relaxed), 0xF000FFFFF000FFFFull);
    check("fetch_xor leaves the difference", a->load(std::memory_order_relaxed),
          0x0FFF00000FFF0000ull);

    check("exchange returns the old value", a->exchange(42, std::memory_order_relaxed),
          0x0FFF00000FFF0000ull);
    check("exchange leaves the new value", a->load(std::memory_order_relaxed), 42);

    // --- compare-exchange, both outcomes -------------------------------------

    uint64_t expected = 42;
    bool swapped = a->compare_exchange_strong(expected, 43, std::memory_order_relaxed);
    check("compare_exchange succeeds when the comparison holds", swapped ? 1 : 0, 1);
    check("a successful compare_exchange stores the desired value",
          a->load(std::memory_order_relaxed), 43);

    expected = 999; // not what is in memory
    swapped = a->compare_exchange_strong(expected, 7, std::memory_order_relaxed);
    check("compare_exchange fails when the comparison does not hold", swapped ? 1 : 0, 0);
    check("a failed compare_exchange reloads `expected` with what was there", expected, 43);
    check("a failed compare_exchange leaves memory alone", a->load(std::memory_order_relaxed), 43);

    // --- a narrower width: the runtime dispatches on size too ----------------

    w->store(0xFFFF0000u, std::memory_order_relaxed);
    check("32-bit fetch_and returns the old value", w->fetch_and(0x00FFFF00u, std::memory_order_relaxed),
          0xFFFF0000u);
    check("32-bit fetch_and leaves the conjunction", w->load(std::memory_order_relaxed), 0x00FF0000u);
    check("32-bit fetch_xor leaves the difference",
          (w->fetch_xor(0x00FF0000u, std::memory_order_relaxed), w->load(std::memory_order_relaxed)),
          0u);

    if (!ok) {
        return 1;
    }
    return csan_expect_no_race().finish();
}
