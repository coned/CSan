// SPDX-License-Identifier: MIT
// Two threads of one machine write different bytes of one line, ordered by release/acquire.
// Expected: no race.
#include "csan_runtime.h"
#include "csan_test.h"

#include <atomic>
#include <thread>

int main() {
    csan_init(1, 0, 1u << 16);
    char* x = static_cast<char*>(csan_alloc(64));
    std::atomic<int> go{0};

    std::thread writer1([&] {
        x[0] = 1;
        go.store(1, std::memory_order_release);
    });
    std::thread writer2([&] {
        while (go.load(std::memory_order_acquire) == 0) {
            std::this_thread::yield();
        }
        x[8] = 2;
    });
    writer1.join();
    writer2.join();
    return csan_expect_no_race().finish();
}
