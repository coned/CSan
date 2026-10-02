// SPDX-License-Identifier: MIT
// Control: the same program with the region left software-coherent.
// Expected: race (write-without-line-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <pthread.h>

namespace {
constexpr uint32_t kHosts = 2;

struct Slot {
    uint32_t meta[2];
    uint8_t payload[56];
};

pthread_mutex_t* sync_region() {
    return static_cast<pthread_mutex_t*>(csan_shared_region(1, sizeof(pthread_mutex_t)));
}

Slot* data_region() {
    return static_cast<Slot*>(csan_shared_region(2, sizeof(Slot)));
}

void declare_scc() {
    csan_register_scc_range(data_region(), sizeof(Slot));
}

int consumer(void*) {
    declare_scc();
    pthread_mutex_t* m = sync_region();
    Slot* slot = data_region();
    pthread_mutex_lock(m);
    _mm_clflushopt(slot);
    _mm_mfence();
    uint8_t out[8];
    std::memcpy(out, slot->payload, sizeof(out));
    slot->meta[0] = 0;
    slot->meta[1] = 0;
    pthread_mutex_unlock(m);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    pthread_mutex_t* m = sync_region();
    Slot* slot = data_region();
    declare_scc();

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(m, &attr);

    pthread_mutex_lock(m);
    std::memset(slot->payload, 1, sizeof(slot->payload));
    slot->meta[0] = 8;
    _mm_clwb(slot);
    _mm_sfence();
    pthread_mutex_unlock(m);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, nullptr);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }

    pthread_mutex_lock(m);
    std::memset(slot->payload, 2, sizeof(slot->payload));
    _mm_clwb(slot);
    _mm_sfence();
    pthread_mutex_unlock(m);

    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE).finish();
}
