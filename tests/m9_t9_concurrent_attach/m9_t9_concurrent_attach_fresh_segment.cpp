// SPDX-License-Identifier: MIT
// Nine machines attach to a not-yet-created segment at once; exactly one may initialize it.
// Expected: no race; 288 bytes written and read.
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

static constexpr uint32_t kHosts = 9;

static int touch_own_word(uint32_t machine) {
    volatile uint64_t* w = static_cast<volatile uint64_t*>(csan_alloc(8));
    uint64_t sum = 0;
    for (uint64_t i = 0; i < 4; ++i) {
        w[0] = machine * 100 + i;
        sum += w[0];
    }
    return csan_test_eq("own word", sum, machine * 400 + 6) ? 0 : 1;
}

int main() {
    char name[64];
    std::snprintf(name, sizeof(name), "/csan_concurrent_attach_%ld", static_cast<long>(getpid()));
    shm_unlink(name);
    setenv("CSAN_SHM_NAME", name, 1);

    int gate[2];
    if (pipe(gate) != 0) {
        std::perror("pipe");
        return 1;
    }
    pid_t kids[kHosts];
    for (uint32_t m = 1; m < kHosts; ++m) {
        kids[m] = fork();
        if (kids[m] < 0) {
            std::perror("fork");
            return 1;
        }
        if (kids[m] == 0) {
            csan_test_arm_timeout();
            close(gate[1]);
            char c;
            ssize_t n = read(gate[0], &c, 1); // EOF once the driver closes its end
            (void)n;
            csan_init(kHosts, m, 1u << 16);
            int rc = touch_own_word(m);
            fflush(nullptr);
            _exit(rc); // no atexit: only the driver unlinks the segment
        }
    }
    close(gate[0]);
    close(gate[1]);
    csan_init(kHosts, 0, 1u << 16);
    int rc = touch_own_word(0);
    for (uint32_t m = 1; m < kHosts; ++m) {
        if (csan_test_wait(kids[m]) != 0) {
            rc = 1;
        }
    }
    shm_unlink(name);
    if (rc != 0) {
        return rc;
    }
    return csan_expect_no_race().writes(288).reads(288).finish();
}
