// SPDX-License-Identifier: MIT
// A CAS spinlock per object over more lock objects than the default clock table holds.
// Expected: no race (run with CSAN_CLOCK_ENTRIES raised).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <immintrin.h>
#include <new>
#include <thread>

namespace {

// More distinct lock objects than the runtime's clock table can hold.
constexpr uint32_t kLocks = 5000;
constexpr uint64_t kLatchBit = 1ull << 63;

struct Meta {
    std::atomic<uint64_t> atomic_word{0};

    void lock() {
    retry:
        uint64_t before = atomic_word.load(std::memory_order_acquire);
        uint64_t after = before | kLatchBit;
        if ((before & kLatchBit) == 0) {
            if (atomic_word.compare_exchange_strong(before, after)) {
                return;
            } else {
                goto retry;
            }
        } else {
            goto retry;
        }
    }

    void unlock() {
        uint64_t before = atomic_word.load(std::memory_order_acquire);
        uint64_t after = before & ~kLatchBit;
        atomic_word.store(after, std::memory_order_release);
    }
};

Meta* g_meta[kLocks];
char* g_x[kLocks];

} // namespace

int main() {
    csan_init(1, 0, 1u << 21);

    for (uint32_t i = 0; i < kLocks; ++i) {
        g_meta[i] = new (csan_alloc(sizeof(Meta))) Meta();
        g_x[i] = static_cast<char*>(csan_alloc(8));
        if (g_meta[i] == nullptr || g_x[i] == nullptr) {
            std::fprintf(stderr, "pool exhausted at %u\n", i);
            return 1;
        }
    }

    volatile int go = 0; // plain HWCC flag, used only to order the test

    // Both threads start up front: neither inherits the other's progress.
    std::thread writer([&] {
        const char payload[8] = {42, 0, 0, 0, 0, 0, 0, 0};
        for (uint32_t i = 0; i < kLocks; ++i) {
            g_meta[i]->lock();
            std::memcpy(g_x[i], payload, sizeof(payload));
            // The fence advances the writer's clock per object, so a dropped clock is observable.
            _mm_clwb(g_x[i]);
            _mm_sfence();
            g_meta[i]->unlock();
        }
        go = 1;
    });
    std::thread reader([&] {
        while (!go) {
            std::this_thread::yield();
        }
        char buf[8];
        for (uint32_t i = 0; i < kLocks; ++i) {
            g_meta[i]->lock();
            std::memcpy(buf, g_x[i], sizeof(buf));
            g_meta[i]->unlock();
        }
    });
    writer.join();
    reader.join();
    return csan_test_finish(false);
}
