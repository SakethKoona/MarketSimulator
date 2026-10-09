#include "mpmc_ring.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    // Four producers, two consumers, every value delivered exactly once.
    constexpr int P = 4, C = 2;
    constexpr std::uint64_t N = 500'000; // per producer
    MpmcRing<std::uint64_t> ring(4096);
    std::atomic<std::uint64_t> consumed{0};
    std::vector<std::atomic<std::uint64_t>> seen_sum(P);
    for (auto &s : seen_sum) s.store(0);
    std::vector<std::thread> ts;
    for (int p = 0; p < P; ++p)
        ts.emplace_back([&, p] {
            for (std::uint64_t i = 1; i <= N; ++i) {
                std::uint64_t v = (static_cast<std::uint64_t>(p) << 40) | i;
                while (!ring.try_push(v)) {}
            }
        });
    std::atomic<bool> done{false};
    for (int c = 0; c < C; ++c)
        ts.emplace_back([&] {
            std::uint64_t v;
            for (;;) {
                if (ring.try_pop(v)) {
                    seen_sum[v >> 40].fetch_add(v & ((1ull << 40) - 1));
                    consumed.fetch_add(1);
                } else if (done.load() && consumed.load() == P * N) {
                    break;
                }
            }
        });
    for (int p = 0; p < P; ++p) ts[p].join();
    done.store(true);
    for (int c = 0; c < C; ++c) ts[P + c].join();
    assert(consumed.load() == P * N);
    for (int p = 0; p < P; ++p) assert(seen_sum[p].load() == N * (N + 1) / 2);
    assert(ring.size_approx() == 0);
    std::printf("mpmc_ring_test: OK  %llu items, %d producers, %d consumers, spins while full=%llu\n",
                (unsigned long long)(P * N), P, C, (unsigned long long)ring.dropped());
    return 0;
}
