// SPDX-License-Identifier: MIT
// Ring-slot reuse: the producer writes back a line whose metadata the consumer updated, without acquiring it first.
// Expected: race (write-without-line-acquire).
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

struct Shared {
    pthread_mutex_t m; // orders the two machines; its own line
    char pad[64 - sizeof(pthread_mutex_t) % 64];
    Slot slot;
};

Shared* region() {
    return static_cast<Shared*>(csan_shared_region(1, sizeof(Shared)));
}

// The consumer: acquire the line, read the payload, reset the metadata.
int consumer(void*) {
    Shared* s = region();
    pthread_mutex_lock(&s->m);
    _mm_clflushopt(&s->slot);
    _mm_mfence();
    uint8_t out[8];
    std::memcpy(out, s->slot.payload, sizeof(out));
    s->slot.meta[0] = 0; // plain, never written back
    s->slot.meta[1] = 0;
    pthread_mutex_unlock(&s->m);
    return 0;
}
} // namespace

int main() {
    csan_init(kHosts, 0, 1u << 16);
    Shared* s = region();
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s->m, &attr);

    // Fill the slot once and publish it, so the consumer has something to read.
    pthread_mutex_lock(&s->m);
    std::memset(s->slot.payload, 1, sizeof(s->slot.payload));
    s->slot.meta[0] = 8;
    _mm_clwb(&s->slot);
    _mm_sfence();
    pthread_mutex_unlock(&s->m);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &consumer, nullptr);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }

    // Reuse the slot: write the payload and write the line back, with no
    // acquire of the line the consumer just wrote.
    pthread_mutex_lock(&s->m);
    std::memset(s->slot.payload, 2, sizeof(s->slot.payload));
    _mm_clwb(&s->slot);
    _mm_sfence();
    pthread_mutex_unlock(&s->m);

    return csan_expect_race(CSAN_RACE_WRITE_WITHOUT_LINE_ACQUIRE)
        .no_kind(CSAN_RACE_WRITE_BEFORE_HB_LINE_WRITE)
        .finish();
}
