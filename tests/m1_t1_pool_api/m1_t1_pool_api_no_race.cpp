// SPDX-License-Identifier: MIT
// Exercises the pool API contract: alloc, realloc, free, shared_region, range calls, is_cxl.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstring>

namespace {

bool ok = true;

void check(const char* what, uint64_t got, uint64_t want) {
    ok = csan_test_eq(what, got, want) && ok;
}

void check_true(const char* what, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ok = false;
    }
}

} // namespace

int main() {
    csan_init(1, 0, 1u << 16);

    // --- the pool is where the header says it is -----------------------------

    void* base = __csan_pool_base();
    uint64_t size = __csan_pool_size();
    check_true("the pool has a base once initialized", base != nullptr);
    check_true("the pool has a size once initialized", size > 0);

    char* p = static_cast<char*>(csan_alloc(64));
    check_true("csan_alloc returns memory", p != nullptr);
    check_true("an allocation lies inside the pool",
               p >= static_cast<char*>(base) && p + 64 <= static_cast<char*>(base) + size);
    check("an allocation is CXL memory", static_cast<uint64_t>(__csan_is_cxl(p, 64)), 1);

    int on_stack = 0;
    check("a stack address is not CXL memory",
          static_cast<uint64_t>(__csan_is_cxl(&on_stack, sizeof(on_stack))), 0);

    // --- realloc keeps the bytes --------------------------------------------

    std::memset(p, 0xAB, 64);
    char* q = static_cast<char*>(csan_realloc(p, 128));
    check_true("csan_realloc returns memory", q != nullptr);
    bool preserved = true;
    for (int i = 0; i < 64; ++i) {
        preserved = preserved && q[i] == static_cast<char>(0xAB);
    }
    check_true("csan_realloc preserves the old contents", preserved);
    check("memory from csan_realloc is CXL memory", static_cast<uint64_t>(__csan_is_cxl(q, 128)), 1);

    // Bump allocator: free reclaims nothing but must not disturb later allocations.
    csan_free(q);
    char* r = static_cast<char*>(csan_alloc(32));
    check_true("csan_alloc still works after a free", r != nullptr);

    // --- shared_region hands the same region back ----------------------------

    void* region = csan_shared_region(7, 256);
    check_true("csan_shared_region carves a region", region != nullptr);
    check_true("csan_shared_region returns the same region for the same id",
               csan_shared_region(7, 256) == region);
    check_true("a region asked for larger than it was made is refused",
               csan_shared_region(7, 1u << 20) == nullptr);

    // --- the range calls: accept pool memory, refuse anything else -----------
    csan_register_range(r, 32);
    csan_register_scc_range(r, 32);
    check("a registered range is still CXL memory",
          static_cast<uint64_t>(__csan_is_cxl(r, 32)), 1);
    csan_unregister_range(r);
    check("unregistering does not take memory out of the pool",
          static_cast<uint64_t>(__csan_is_cxl(r, 32)), 1);
    // Refused with a message on stderr rather than accepted or fatal.
    csan_register_range(&on_stack, sizeof(on_stack));

    // Pool traffic through this memory must be observed as events.
    r[0] = 7;
    volatile char v = r[0];
    (void)v;

    if (!ok) {
        return 1;
    }
    return csan_expect_no_race().finish();
}
