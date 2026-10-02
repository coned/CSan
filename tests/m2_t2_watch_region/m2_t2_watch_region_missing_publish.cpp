// SPDX-License-Identifier: MIT
// Watched (not checker-allocated) memory, with the writer's sfence omitted.
// Expected: race (read-without-byte-acquire).
#include "csan_runtime.h"
#include "csan_test.h"

#include <cstdio>
#include <fcntl.h>
#include <immintrin.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr uint32_t kRegionId = 7;
constexpr size_t kRegionBytes = 4096;

struct Ctx {
    int fd;
    char* writer_view;
};

// Map the region afresh in this process and tell the checker where it landed.
// mmap picks the address, so the two machines disagree about it; the checker
// keys the region's state by (id, offset) and does not care.
char* map_and_watch(int fd) {
    void* p = mmap(nullptr, kRegionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        return nullptr;
    }
    csan_watch(kRegionId, p, kRegionBytes);
    return static_cast<char*>(p);
}

int reader(void* arg) {
    Ctx* c = static_cast<Ctx*>(arg);
    char* x = map_and_watch(c->fd);
    if (x == nullptr || x == c->writer_view) {
        std::fprintf(stderr, "machine 1: wanted a mapping distinct from machine 0's\n");
        return 1;
    }
    _mm_clflushopt(x);         // flushopt x
    _mm_mfence();              // acquire
    volatile char v = x[0];    // read x
    (void)v;
    return 0;
}

} // namespace

int main() {
    // Shadow for watched regions is budgeted when the segment is created, so
    // this has to be set before the first call that attaches.
    setenv("CSAN_WATCH_BYTES", "65536", 1);
    constexpr uint32_t kHosts = 2;
    csan_init(kHosts, 0, 1u << 16);

    char name[64];
    std::snprintf(name, sizeof(name), "/csan_watch_%d", static_cast<int>(getpid()));
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, kRegionBytes) != 0) {
        std::fprintf(stderr, "machine 0: could not create the shared object\n");
        return 1;
    }
    // The fd keeps the object alive across the fork, so unlinking now leaves
    // nothing behind when a repetition ends.
    shm_unlink(name);

    Ctx ctx;
    ctx.fd = fd;
    ctx.writer_view = map_and_watch(fd);
    if (ctx.writer_view == nullptr) {
        std::fprintf(stderr, "machine 0: could not map the region\n");
        return 1;
    }

    ctx.writer_view[0] = 42;    // write x
    _mm_clwb(ctx.writer_view);  // writeback x
    // missing: _mm_sfence();   // publish

    pid_t child = csan_test_spawn_machine(kHosts, 1, &reader, &ctx);
    if (csan_test_wait(child) != 0) {
        std::fprintf(stderr, "machine 1 role failed\n");
        return 1;
    }
    return csan_expect_race(CSAN_RACE_READ_WITHOUT_BYTE_ACQUIRE).finish();
}
