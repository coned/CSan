// SPDX-License-Identifier: MIT
// A one-byte read splits a word; an unordered whole-word write must still see that read.
// Expected: race (write after read).
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <cstdint>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(8));
    std::atomic<bool> read_done{false}; // HWCC, relaxed: orders the test, not the checker

    std::thread reader([&] {
        volatile char v = x[3]; // one byte: splits the word
        (void)v;
        read_done.store(true, std::memory_order_relaxed);
    });
    while (!read_done.load(std::memory_order_relaxed)) {
        std::this_thread::yield();
    }
    *reinterpret_cast<volatile uint64_t*>(x) = 42; // the whole word, no edge from the read
    reader.join();
    return csan_expect_race(CSAN_RACE_WRITE_AFTER_READ).finish();
}
