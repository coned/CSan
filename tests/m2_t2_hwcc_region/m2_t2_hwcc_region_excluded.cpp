// SPDX-License-Identifier: MIT
// Ring-slot reuse hazard inside a region declared hardware-coherent.
// Expected: no race (the region is not checked).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <pthread.h>

namespace {
constexpr uint32_t kHosts = 2;

struct Slot {          // one 64-byte line
    uint32_t meta[2];  // remaining_size, dequeue_offset
    uint8_t payload[56];
};

// Two regions, so the declaration can name the data without also naming the
// lock that orders the machines.
pthread_mutex_t* sync_region() {
    return static_cast<pthread_mutex_t*>(csan_shared_region(1, sizeof(pthread_mutex_t)));
}

Slot* data_region() {
    return static_cast<Slot*>(csan_shared_region(2, sizeof(Slot)));
}

// Declared by every machine. The flag lives in the shared segment, so the
// first call is what matters and the rest are idempotent.
void declare_hwcc() {
    csan_register_hwcc_range(data_region(), sizeof(Slot));
}

// The consumer: acquire the line, read the payload, reset the metadata with
// plain stores that are never written back.
int consumer(void*) {
    declare_hwcc();
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
    declare_hwcc();

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(m, &attr);

    // Fill the slot once and publish it, so the consumer has something to read.
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

    // Reuse the slot with no acquire of the line the consumer just wrote.
    pthread_mutex_lock(m);
    std::memset(slot->payload, 2, sizeof(slot->payload));
    _mm_clwb(slot);
    _mm_sfence();
    pthread_mutex_unlock(m);

    return csan_expect_no_race().finish();
}
