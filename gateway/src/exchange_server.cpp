// exchange_server: runs the matching engine with the feed publisher and a
// synthetic order-flow generator, until SIGINT.
// Usage: exchange_server [config.json] [--rate N] [--seconds S] [--quiet]
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
    double rate = 1000; // orders per second
    double seconds = 0; // 0 = until SIGINT
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
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

    FeedConfig fcfg = feed_config_from(cfg);
    FeedPublisher pub(ex.Sink(), table, fcfg);
    pub.start();
    std::fprintf(stderr, "exchange_server: feed on %s:%u, %zu symbols, %.0f orders/s\n",
                 fcfg.group.c_str(), fcfg.port, ids.size(), rate);

    SpscRing<InboundCommand> commands(1 << 14);
    SpscRing<OrderReport> reports(1 << 16);
    EngineLoop loop(ex, commands, reports);
    BoeConfig bcfg = boe_config_from(cfg);
    BoeServer boe(bcfg, commands, reports, ex.Symbols());
    boe.start();
    std::fprintf(stderr, "exchange_server: BOE order entry on %s:%u\n", bcfg.bind.c_str(), bcfg.port);

    FlowGenerator gen(ex, ids, 42);
    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto last_report = start;
    std::uint64_t orders = 0;
    const double ns_per_order = rate > 0 ? 1e9 / rate : 0;

    while (!g_stop) {
        auto now = clock::now();
        double elapsed_ns = std::chrono::duration<double, std::nano>(now - start).count();
        if (seconds > 0 && elapsed_ns >= seconds * 1e9)
            break;
        bool worked = loop.pump() > 0;
        if (rate > 0 && orders * ns_per_order <= elapsed_ns) {
            gen.step();
            ++orders;
            worked = true;
        }
        if (!worked)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        if (!quiet && now - last_report >= std::chrono::seconds(1)) {
            last_report = now;
            const auto &s = pub.stats();
            std::fprintf(stderr,
                         "[%6.1fs] orders=%llu live=%zu | feed msgs=%llu pkts=%llu hb=%llu "
                         "next_seq=%llu drops=%llu unpaired=%llu send_err=%llu errno=%d | "
                         "boe sess=%llu in=%llu rej=%llu out=%llu\n",
                         elapsed_ns / 1e9, (unsigned long long)orders, gen.live(),
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
    boe.stop();
    pub.stop();
    const auto &s = pub.stats();
    std::fprintf(stderr, "exchange_server: done. orders=%llu msgs=%llu pkts=%llu drops=%llu\n",
                 (unsigned long long)orders, (unsigned long long)s.messages.load(),
                 (unsigned long long)s.packets.load(), (unsigned long long)s.engine_drops.load());
    return 0;
}
