// SPDX-License-Identifier: MIT
// Machine 0 writes x after forking machine 1, which reads it.
// Expected: race (the fork edge carries the clock as of the fork).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <unistd.h>

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));

    pid_t child = fork();
    if (child < 0) {
        std::perror("fork");
        return 1;
    }
    if (child == 0) {
        usleep(100 * 1000); // let the parent write first, in the window
        csan_spawn(kHosts, 1);
        volatile char v = x[0];
        (void)v;
        fflush(nullptr);
        _exit(0);
    }
    x[0] = 42; // after the fork: not ordered before anything the child does
    if (csan_test_wait(child) != 0) {
        return 1;
    }
    return csan_test_finish(true);
}
