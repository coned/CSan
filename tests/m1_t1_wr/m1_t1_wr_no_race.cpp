// SPDX-License-Identifier: MIT
// One thread writes then reads a pool byte.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    x[0] = 42;              // write x
    volatile char v = x[0]; // read x
    (void)v;
    return csan_test_finish(false);
}
