// exchange_server: runs the matching engine with the feed publisher and a
// synthetic order-flow generator, until SIGINT.
// Usage: exchange_server [config.json] [--rate N] [--seconds S] [--quiet] [--capture FILE]
// --rate N enables the in-process synthetic generator; the default is 0,
// order flow is expected over BOE (see tools/flowgen.cpp).
#include "exchange.hpp"
#include "boe_server.hpp"
#include "engine_loop.hpp"
#include "feed_publisher.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static FeedConfig feed_config_from(const json &cfg) {
    FeedConfig f;
    if (!cfg.contains("feed"))
        return f;
    const json &j = cfg["feed"];
    if (j.contains("group")) f.group = j["group"].get<std::string>();
    if (j.contains("port")) f.port = j["port"].get<std::uint16_t>();
    if (j.contains("interface")) f.interface = j["interface"].get<std::string>();
    if (j.contains("session")) f.session = j["session"].get<std::string>();
    if (j.contains("flush_us")) f.flush_us = j["flush_us"].get<std::uint32_t>();
    if (j.contains("heartbeat_ms")) f.heartbeat_ms = j["heartbeat_ms"].get<std::uint32_t>();
    if (j.contains("directory_s")) f.directory_s = j["directory_s"].get<std::uint32_t>();
    if (j.contains("ttl")) f.ttl = j["ttl"].get<int>();
    if (j.contains("loop")) f.loop = j["loop"].get<bool>();
    if (j.contains("retransmit_capacity"))
        f.retransmit_capacity = j["retransmit_capacity"].get<std::size_t>();
    return f;
}

static BoeConfig boe_config_from(const json &cfg) {
    BoeConfig b;
    if (!cfg.contains("boe"))
        return b;
    const json &j = cfg["boe"];
    if (j.contains("port")) b.port = j["port"].get<std::uint16_t>();
    if (j.contains("bind")) b.bind = j["bind"].get<std::string>();
    if (j.contains("heartbeat_ms")) b.heartbeat_ms = j["heartbeat_ms"].get<std::uint32_t>();
    if (j.contains("timeout_ms")) b.timeout_ms = j["timeout_ms"].get<std::uint32_t>();
    if (j.contains("poll_ms")) b.poll_ms = j["poll_ms"].get<std::uint32_t>();
    if (j.contains("max_sessions")) b.max_sessions = j["max_sessions"].get<std::size_t>();
    return b;
}

// Random order flow: a slow random walk per symbol, limit orders scattered
// around it, cancels of live orders, and some aggressive IOCs.
class FlowGenerator {
  public:
    FlowGenerator(Exchange &ex, std::vector<SymbolId> syms, std::uint64_t seed)
        : ex_(ex), syms_(std::move(syms)), rng_(seed), mid_(syms_.size(), 10000) {}

    void step() {
        std::size_t si = rng_() % syms_.size();
        SymbolId sym = syms_[si];
        std::uint64_t &mid = mid_[si];
        if (rng_() % 20 == 0)
            mid += (rng_() % 3) - 1; // drift
        unsigned r = rng_() % 100;
        if (r < 55) { // passive limit
            Side side = (rng_() % 2) ? Side::Buy : Side::Sell;
            std::uint64_t off = 1 + rng_() % 15;
            Price px = side == Side::Buy ? mid - off : mid + off;
            Quantity q = 1 + rng_() % 500;
            auto res = ex_.SubmitOrder(sym, px, q, side);
            if (res.resting)
                live_.push_back(res.order_id);
        } else if (r < 80 && !live_.empty()) { // cancel
            std::size_t i = rng_() % live_.size();
            ex_.CancelOrder(live_[i]);
            live_[i] = live_.back();
            live_.pop_back();
        } else if (r < 90 && !live_.empty()) { // modify down
            std::size_t i = rng_() % live_.size();
            ex_.ModifyOrder(live_[i], 1 + rng_() % 50);
        } else { // aggressive IOC crossing the spread
            Side side = (rng_() % 2) ? Side::Buy : Side::Sell;
            Price px = side == Side::Buy ? mid + 20 : (mid > 20 ? mid - 20 : 1);
            Quantity q = 1 + rng_() % 800;
            ex_.SubmitOrder(sym, px, q, side, OrderType::LIMIT, TypeInForce::IOC);
        }
        if (live_.size() > 20000) { // keep the book bounded
            ex_.CancelOrder(live_.front());
            live_.front() = live_.back();
            live_.pop_back();
        }
    }
    std::size_t live() const { return live_.size(); }

  private:
    Exchange &ex_;
    std::vector<SymbolId> syms_;
    std::mt19937_64 rng_;
    std::vector<std::uint64_t> mid_;
    std::vector<OrderId> live_;
};

int main(int argc, char **argv) {
    std::string config_path;
    double rate = 0; // in-process synthetic orders per second; 0 = none (use flowgen over BOE)
    double seconds = 0; // 0 = until SIGINT
    bool quiet = false;
    std::string capture;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else config_path = argv[i];
    }
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    json cfg = json::parse(std::ifstream(config_path.empty() ? "configs/default.json" : config_path));
    Exchange ex(cfg);

    FeedPublisher::SymbolTable table;
    std::vector<SymbolId> ids;
    for (const auto &[name, id] : ex.Symbols()) {
        table.emplace_back(id, name);
        ids.push_back(id);
    }
    std::sort(table.begin(), table.end());
    std::sort(ids.begin(), ids.end());


    // One engine thread per shard. Each owns its EngineLoop, its command and
    // report rings, and a synthetic generator restricted to its symbols, so
    // no shard is ever touched by two threads.
    const std::size_t shards = ex.NumShards();
    std::vector<std::unique_ptr<SpscRing<InboundCommand>>> commands;
    std::vector<std::unique_ptr<SpscRing<OrderReport>>> reports;
    std::vector<SpscRing<InboundCommand> *> cmd_ptrs;
    std::vector<SpscRing<OrderReport> *> rep_ptrs;
    std::vector<EventSink *> sinks;
    std::vector<std::vector<SymbolId>> shard_syms(shards);
    std::vector<std::uint8_t> symbol_shard(ids.empty() ? 0 : ids.back() + 1, 0);
    for (std::size_t i = 0; i < shards; ++i) {
        commands.push_back(std::make_unique<SpscRing<InboundCommand>>(1 << 16));
        reports.push_back(std::make_unique<SpscRing<OrderReport>>(1 << 16));
        cmd_ptrs.push_back(commands.back().get());
        rep_ptrs.push_back(reports.back().get());
        sinks.push_back(&ex.Sink(i));
    }
    for (SymbolId id : ids) {
        std::uint8_t sh = ex.ShardOf(id);
        symbol_shard[id] = sh;
        shard_syms[sh].push_back(id);
    }

    FeedConfig fcfg = feed_config_from(cfg);
    fcfg.capture_path = capture;
    FeedPublisher pub(sinks, table, fcfg);
    pub.start();
    std::fprintf(stderr, "exchange_server: feed on %s:%u, %zu symbols, %zu shard(s), %.0f orders/s\n",
                 fcfg.group.c_str(), fcfg.port, ids.size(), shards, rate);

    BoeConfig bcfg = boe_config_from(cfg);
    BoeServer boe(bcfg, cmd_ptrs, rep_ptrs, ex.Symbols(), symbol_shard);
    boe.start();
    std::fprintf(stderr, "exchange_server: BOE order entry on %s:%u\n", bcfg.bind.c_str(), bcfg.port);

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total_orders{0};
    std::atomic<std::uint64_t> total_live{0};
    std::vector<std::thread> threads;
    const double shard_rate = rate / static_cast<double>(shards);
    const double ns_per_order = shard_rate > 0 ? 1e9 / shard_rate : 0;

    for (std::size_t sh = 0; sh < shards; ++sh) {
        threads.emplace_back([&, sh] {
            EngineLoop loop(ex, sh, *commands[sh], *reports[sh]);
            FlowGenerator gen(ex, shard_syms[sh], 42 + sh);
            std::uint64_t orders = 0, last_live = 0;
            const bool generate = rate > 0 && !shard_syms[sh].empty();
            while (!stop.load(std::memory_order_relaxed)) {
                bool worked = loop.pump() > 0;
                if (generate) {
                    double elapsed_ns = std::chrono::duration<double, std::nano>(clock::now() - start).count();
                    if (orders * ns_per_order <= elapsed_ns) {
                        gen.step();
                        ++orders;
                        total_orders.fetch_add(1, std::memory_order_relaxed);
                        worked = true;
                    }
                }
                std::uint64_t live = gen.live();
                if (live != last_live) {
                    total_live.fetch_add(live - last_live, std::memory_order_relaxed);
                    last_live = live;
                }
                if (!worked)
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        });
    }

    auto last_report = start;
    while (!g_stop) {
        auto now = clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(now - start).count();
        if (seconds > 0 && elapsed_ns >= seconds * 1e9)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!quiet && now - last_report >= std::chrono::seconds(1)) {
            last_report = now;
            const auto &s = pub.stats();
            std::fprintf(stderr,
                         "[%6.1fs] orders=%llu live=%llu | feed msgs=%llu pkts=%llu hb=%llu "
                         "next_seq=%llu drops=%llu unpaired=%llu send_err=%llu errno=%d | "
                         "boe sess=%llu in=%llu rej=%llu out=%llu\n",
                         elapsed_ns / 1e9, (unsigned long long)total_orders.load(),
                         (unsigned long long)total_live.load(),
                         (unsigned long long)s.messages.load(), (unsigned long long)s.packets.load(),
                         (unsigned long long)s.heartbeats.load(), (unsigned long long)s.next_seq.load(),
                         (unsigned long long)s.engine_drops.load(),
                         (unsigned long long)s.unpaired_executes.load(),
                         (unsigned long long)s.send_errors.load(), s.last_errno.load(),
                         (unsigned long long)boe.stats().sessions.load(),
                         (unsigned long long)(boe.stats().orders_in.load() + boe.stats().cancels_in.load() + boe.stats().modifies_in.load()),
                         (unsigned long long)boe.stats().rejects.load(),
                         (unsigned long long)boe.stats().reports_out.load());
        }
    }
    stop.store(true);
    for (auto &t : threads)
        t.join();
    const std::uint64_t orders = total_orders.load();

    // With --capture, also dump the final books so a client that replays
    // the capture can check its reconstruction: one line per level,
    // "symbol_id side price qty order_count", bids then asks per symbol.
    if (!capture.empty()) {
        std::string path = capture + ".books.txt";
        if (FILE *f = std::fopen(path.c_str(), "w")) {
            for (SymbolId id : ids) {
                const OrderBook &b = ex.GetBook(id);
                for (auto *n = b.bids().GetHead(); n; n = n->forward[0])
                    std::fprintf(f, "%llu B %llu %llu %d\n", (unsigned long long)id,
                                 (unsigned long long)n->value.price,
                                 (unsigned long long)n->value.TotalQuantity(), n->value.GetSize());
                for (auto *n = b.asks().GetHead(); n; n = n->forward[0])
                    std::fprintf(f, "%llu S %llu %llu %d\n", (unsigned long long)id,
                                 (unsigned long long)n->value.price,
                                 (unsigned long long)n->value.TotalQuantity(), n->value.GetSize());
            }
            std::fclose(f);
            std::fprintf(stderr, "exchange_server: wrote %s\n", path.c_str());
        }
    }
    boe.stop();
    pub.stop();
    const auto &s = pub.stats();
    std::fprintf(stderr, "exchange_server: done. orders=%llu msgs=%llu pkts=%llu drops=%llu\n",
                 (unsigned long long)orders, (unsigned long long)s.messages.load(),
                 (unsigned long long)s.packets.load(), (unsigned long long)s.engine_drops.load());
    return 0;
}
