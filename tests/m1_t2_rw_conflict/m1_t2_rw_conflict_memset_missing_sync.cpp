// SPDX-License-Identifier: MIT
// A memset concurrent with a read, with no edge.
// Expected: race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstring>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    volatile char* vx = x;

    std::thread reader([&] {
        volatile char v = vx[0];
        (void)v;
    });
    std::memset(x, 7, 8); // concurrent with the read, no edge
    reader.join();
    return csan_test_finish(true);
}
