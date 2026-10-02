// SPDX-License-Identifier: MIT
// Attaches and creates a thread from a static constructor, before the runtime's own initializers.
// Expected: no race, and no crash.
#include "csan_runtime.h"
#include "csan_test.h"

#include <pthread.h>

static char* g_x = nullptr;

static void* writer(void*) {
    g_x[0] = 1;
    return nullptr;
}

__attribute__((constructor)) static void attach_before_main() {
    csan_init(1, 0, 1u << 16);
    g_x = static_cast<char*>(csan_alloc(8));
    pthread_t th;
    if (pthread_create(&th, nullptr, writer, nullptr) != 0) {
        _exit(1);
    }
    pthread_join(th, nullptr);
}

int main() {
    volatile char v = g_x[0];
    (void)v;
    return csan_test_finish(false);
}
