// SPDX-License-Identifier: MIT
// Missing sfence reached through several call layers, to exercise stack capture.
// Expected: race (read-without-byte-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <immintrin.h>

namespace {

struct Ring {
    char* slot;
};

// Each layer is noinline so the stack is real at any optimization level.
#define LAYER __attribute__((noinline))

// ---- machine 0: the producer ----------------------------------------------

LAYER void store_payload(Ring* r) {
    r->slot[0] = 42;
}

LAYER void fill_slot(Ring* r) {
    store_payload(r);
}

LAYER void write_back_slot(Ring* r) {
    _mm_clwb(r->slot);
    // missing: _mm_sfence();
}

LAYER void publish_entry(Ring* r) {
    fill_slot(r);
    write_back_slot(r);
}

LAYER void enqueue(Ring* r) {
    publish_entry(r);
}

LAYER void producer_step(Ring* r) {
    enqueue(r);
}

// ---- machine 1: the consumer ----------------------------------------------

LAYER char load_payload(Ring* r) {
    volatile char v = r->slot[0];
    return v;
}

LAYER char drain_slot(Ring* r) {
    return load_payload(r);
}

LAYER void acquire_slot(Ring* r) {
    _mm_clflushopt(r->slot);
    _mm_mfence();
}

LAYER char consume_entry(Ring* r) {
    acquire_slot(r);
    return drain_slot(r);
}

LAYER char dequeue(Ring* r) {
    return consume_entry(r);
}

LAYER char consumer_step(Ring* r) {
    return dequeue(r);
}

int reader(void* arg) {
    Ring* r = static_cast<Ring*>(arg);
    (void)consumer_step(r);
    return 0;
}

} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);

    Ring ring;
    ring.slot = static_cast<char*>(csan_alloc(8));
    producer_step(&ring);

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ring);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
