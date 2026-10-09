// Throughput benchmark: mixed submit/cancel flow on one symbol.
// Build and run with `make bench`. Deterministic, so numbers are comparable
// across changes on the same machine.
#include "exchange.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
    const int N = argc > 1 ? std::atoi(argv[1]) : 2'000'000;
    const int ROUNDS = 3;

    json cfg = {{"symbols", {{"AAPL", json::object()}}},
                {"sink_size", 1 << 20},
                {"seq_capacity", 1},
                {"num_symbols", 1}};

    double best = 1e18;
    for (int round = 0; round < ROUNDS; round++) {
        Exchange ex(cfg);
        uint64_t x = 42;
        auto rnd = [&] {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            return x;
        };
        std::vector<OrderId> live;
        live.reserve(N);
        long matched = 0;

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < N; i++) {
            int op = rnd() % 10;
            if (op < 7 || live.empty()) {
                auto r = ex.SubmitOrder(SymbolId{0}, 990 + rnd() % 21,
                                        1 + rnd() % 100,
                                        rnd() & 1 ? Side::Buy : Side::Sell);
                if (r.resting)
                    live.push_back(r.order_id);
                if (r.qty_executed)
                    matched++;
            } else {
                size_t k = rnd() % live.size();
                ex.CancelOrder(live[k]);
                live[k] = live.back();
                live.pop_back();
            }
            if ((i & 1023) == 0)
                while (ex.NextEvent()) {
                }
        }
        double dt = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        best = std::min(best, dt);
        std::cout << "round " << round + 1 << ": " << (long)(N / dt)
                  << " ops/s, " << (dt / N * 1e9) << " ns/op  (live="
                  << live.size() << ", matched=" << matched << ")\n";
    }
    std::cout << "best: " << (long)(N / best) << " ops/s, " << (best / N * 1e9)
              << " ns/op\n";
}
