// SPDX-License-Identifier: MIT
// Two threads of one machine write the same byte of one line under a pthread mutex.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <pthread.h>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(64));
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

    std::thread writer1([&] {
        pthread_mutex_lock(&m);
        x[0] = 1;
        pthread_mutex_unlock(&m);
    });
    std::thread writer2([&] {
        pthread_mutex_lock(&m);
        x[0] = 2;
        pthread_mutex_unlock(&m);
    });
    writer1.join();
    writer2.join();
    return csan_expect_no_race().finish();
}
