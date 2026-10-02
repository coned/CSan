// SPDX-License-Identifier: MIT
// Same CAS spinlock, but the reader skips the lock.
// Expected: race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdio>
#include <new>

namespace {

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

struct Ctx {
    Meta* meta;
    char* x;
};

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    // missing: c->meta->lock();
    volatile char v = c->x[0]; // read x, unsynchronized
    (void)v;
    // missing: c->meta->unlock();
    return 0;
}

} // namespace

int main() {
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);
    Ctx ctx;
    ctx.meta = new (csan_alloc(sizeof(Meta))) Meta();
    ctx.x = static_cast<char*>(csan_alloc(8));

    ctx.meta->lock();
    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);

    ctx.x[0] = 42; // write x
    ctx.meta->unlock();

    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_test_finish(true);
}
