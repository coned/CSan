// SPDX-License-Identifier: MIT
// Handoff through a seq_cst store and seq_cst load (the default ordering).
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <new>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* data = static_cast<char*>(csan_alloc(8));
    auto* flag = new (csan_alloc(sizeof(std::atomic<int>))) std::atomic<int>(0);

    std::thread consumer([&] {
        // No ordering named: this is seq_cst.
        while (flag->load() != 1) {
        }
        volatile char v = data[0];
        (void)v;
    });
    data[0] = 42;
    flag->store(1); // seq_cst
    consumer.join();
    return csan_expect_no_race().finish();
}
