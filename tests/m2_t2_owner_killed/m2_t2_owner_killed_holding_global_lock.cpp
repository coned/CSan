// SPDX-License-Identifier: MIT
// Machine 1 is SIGKILLed while holding the segment's global lock; machine 0 then needs the lock.
// Expected: no race and no hang.
#include "csan_runtime.h"
#include "csan_test.h"

#include <csignal>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr uint32_t kHosts = 2;
static char* g_range = nullptr;
static char* g_x = nullptr;

static int churn_ranges(void*) {
    for (;;) {
        csan_register_range(g_range, 64);
        csan_unregister_range(g_range);
    }
}

static void* writer(void*) {
    g_x[0] = 1;
    return nullptr;
}

int main() {
    csan_init(kHosts, 0, 1u << 16);
    g_range = static_cast<char*>(csan_alloc(64));
    g_x = static_cast<char*>(csan_alloc(8));

    pid_t child = csan_test_spawn_machine(kHosts, 1, churn_ranges, nullptr);
    if (child < 0) {
        return 1;
    }
    usleep(20 * 1000);
    kill(child, SIGKILL);
    int status = 0;
    waitpid(child, &status, 0);

    pthread_t th;
    if (pthread_create(&th, nullptr, writer, nullptr) != 0) {
        return 1;
    }
    pthread_join(th, nullptr);
    return csan_expect_no_race().writes(1).finish();
}
