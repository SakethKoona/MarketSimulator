// exchange_server: runs the matching engine with the feed publisher and a
// synthetic order-flow generator, until SIGINT.
// Usage: exchange_server [config.json] [--rate N] [--seconds S] [--quiet] [--capture FILE]
// --rate N enables the in-process synthetic generator; the default is 0,
// order flow is expected over BOE (see tools/flowgen.cpp).
#include "exchange.hpp"
#include "boe_server.hpp"
#include "feed_publisher.hpp"
#include "flowgen_ingress.hpp"
#include "ingress/core.hpp"
#include "ingress/plugin.hpp"
#include "retransmit_server.hpp"
#include "snapshot_service.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <memory>
#include <random>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

// Service plumbing: state dir, pid file, log file, detaching, stopping.
static std::string state_dir() {
    if (const char *x = std::getenv("XDG_STATE_HOME"); x && *x)
        return std::string(x) + "/mktsim";
    if (const char *h = std::getenv("HOME"); h && *h)
        return std::string(h) + "/.local/state/mktsim";
    return "/tmp/mktsim";
}
static void mkdirs(const std::string &dir) {
    std::string cur;
    for (std::size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == '/' || i + 1 == dir.size())
            ::mkdir(cur.c_str(), 0755);
    }
}
static pid_t read_pid(const std::string &pidfile) {
    FILE *f = std::fopen(pidfile.c_str(), "r");
    if (!f)
        return 0;
    long pid = 0;
    if (std::fscanf(f, "%ld", &pid) != 1)
        pid = 0;
    std::fclose(f);
    return static_cast<pid_t>(pid);
}
static bool alive(pid_t pid) { return pid > 0 && ::kill(pid, 0) == 0; }

static int do_stop(const std::string &pidfile) {
    pid_t pid = read_pid(pidfile);
    if (!alive(pid)) {
        std::fprintf(stderr, "exchange_server: not running (%s)\n", pidfile.c_str());
        ::unlink(pidfile.c_str());
        return 1;
    }
    ::kill(pid, SIGTERM);
    for (int i = 0; i < 100 && alive(pid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (alive(pid)) {
        std::fprintf(stderr, "exchange_server: pid %ld did not exit, sending SIGKILL\n", (long)pid);
        ::kill(pid, SIGKILL);
    }
    ::unlink(pidfile.c_str());
    std::fprintf(stderr, "exchange_server: stopped pid %ld\n", (long)pid);
    return 0;
}

static int do_status(const std::string &pidfile, const std::string &logfile) {
    pid_t pid = read_pid(pidfile);
    if (!alive(pid)) {
        std::printf("stopped\n");
        return 1;
    }
    std::printf("running pid %ld\nlog %s\n", (long)pid, logfile.c_str());
    return 0;
}

// Double-fork, new session, stdio to the log. Returns in the daemon child
// only; the parent prints the pid and exits.
static void daemonize(const std::string &pidfile, const std::string &logfile) {
    pid_t pid = ::fork();
    if (pid < 0) { std::perror("fork"); std::exit(1); }
    if (pid > 0) {
        // Wait briefly for the grandchild's pid file so the caller can read it.
        for (int i = 0; i < 50; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (pid_t p = read_pid(pidfile); alive(p)) {
                std::fprintf(stderr, "exchange_server: started pid %ld, log %s\n", (long)p, logfile.c_str());
                std::exit(0);
            }
        }
        std::fprintf(stderr, "exchange_server: started (pid file not yet visible), log %s\n", logfile.c_str());
        std::exit(0);
    }
    if (::setsid() < 0) std::exit(1);
    pid = ::fork();
    if (pid < 0) std::exit(1);
    if (pid > 0) ::_exit(0);
    ::umask(022);
    // Drop every descriptor inherited from the parent (pipes, sockets) so
    // the service never pins something the caller is waiting on.
    {
        long max_fd = ::sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 65536) max_fd = 65536;
        for (int i = 3; i < max_fd; ++i)
            ::close(i);
    }
    int fd = ::open(logfile.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > 2) ::close(fd);
    }
    int nul = ::open("/dev/null", O_RDONLY);
    if (nul >= 0) { ::dup2(nul, STDIN_FILENO); if (nul > 2) ::close(nul); }
    ::setvbuf(stderr, nullptr, _IOLBF, 0);
    if (FILE *f = std::fopen(pidfile.c_str(), "w")) {
        std::fprintf(f, "%ld\n", (long)::getpid());
        std::fclose(f);
    }
}

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
    bool daemon = false, stop_cmd = false, status_cmd = false;
    std::string pidfile = state_dir() + "/exchange.pid";
    std::string logfile = state_dir() + "/exchange.log";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--quiet")) quiet = true;
        else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) capture = argv[++i];
        else if (!std::strcmp(argv[i], "--daemon")) daemon = true;
        else if (!std::strcmp(argv[i], "--stop")) stop_cmd = true;
        else if (!std::strcmp(argv[i], "--status")) status_cmd = true;
        else if (!std::strcmp(argv[i], "--pidfile") && i + 1 < argc) pidfile = argv[++i];
        else if (!std::strcmp(argv[i], "--logfile") && i + 1 < argc) logfile = argv[++i];
        else if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) {
            std::printf("exchange_server [config.json] [--daemon | --stop | --status] [--pidfile P] [--logfile L]\n"
                        "                [--rate N] [--seconds S] [--quiet] [--capture FILE]\n"
                        "Runs the exchange until SIGINT/SIGTERM. --daemon detaches (pid and log under\n"
                        "$XDG_STATE_HOME/mktsim or ~/.local/state/mktsim); --stop / --status act on that pid.\n");
            return 0;
        }
        else config_path = argv[i];
    }
    if (stop_cmd)
        return do_stop(pidfile);
    if (status_cmd)
        return do_status(pidfile, logfile);
    if (pid_t other = read_pid(pidfile); daemon && alive(other)) {
        std::fprintf(stderr, "exchange_server: already running as pid %ld (%s)\n", (long)other, pidfile.c_str());
        return 1;
    }
    // Resolve the config before detaching so a bad path fails loudly here.
    std::string resolved = config_path;
    if (resolved.empty()) {
        for (const char *c : {"configs/default.json", "../configs/default.json", "../../configs/default.json"})
            if (std::ifstream(c)) { resolved = c; break; }
        if (resolved.empty()) {
            std::fprintf(stderr, "exchange_server: no config given and configs/default.json not found\n");
            return 2;
        }
    }
    if (daemon) {
        mkdirs(state_dir());
        if (!std::ifstream(resolved)) {
            std::fprintf(stderr, "exchange_server: cannot read %s\n", resolved.c_str());
            return 2;
        }
        // Make the config path absolute: the daemon keeps the cwd, but be safe.
        if (resolved[0] != '/') {
            char cwd[4096];
            if (::getcwd(cwd, sizeof(cwd)))
                resolved = std::string(cwd) + "/" + resolved;
        }
        daemonize(pidfile, logfile);
        quiet = false; // the per-second line is the log
    }
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    std::signal(SIGHUP, SIG_IGN);

    json cfg = json::parse(std::ifstream(resolved));
    std::fprintf(stderr, "exchange_server: config %s\n", resolved.c_str());
    Exchange ex(cfg);

    FeedPublisher::SymbolTable table;
    std::vector<SymbolId> ids;
    for (const auto &[name, id] : ex.Symbols()) {
        table.emplace_back(id, name);
        ids.push_back(id);
    }
    std::sort(table.begin(), table.end());
    std::sort(ids.begin(), ids.end());


    // Ingress: the order-entry core implements the C API; adapters (built-in
    // BOE, or shared libraries) are listed in the config's "ingress" array.
    OrderEntryCore::Config ccfg;
    if (cfg.contains("ingress_core")) {
        const json &j = cfg["ingress_core"];
        if (j.contains("cmd_ring")) ccfg.cmd_ring = j["cmd_ring"].get<std::size_t>();
        if (j.contains("report_ring")) ccfg.report_ring = j["report_ring"].get<std::size_t>();
        if (j.contains("max_sessions")) ccfg.max_sessions = j["max_sessions"].get<std::size_t>();
    }
    OrderEntryCore core(ex, ccfg);
    const std::size_t shards = core.num_shards();
    std::vector<EventSink *> sinks;
    std::vector<std::vector<SymbolId>> shard_syms(shards);
    for (std::size_t i = 0; i < shards; ++i)
        sinks.push_back(&ex.Sink(i));
    for (SymbolId id : ids)
        shard_syms[ex.ShardOf(id)].push_back(id);

    FeedConfig fcfg = feed_config_from(cfg);
    fcfg.capture_path = capture;
    FeedPublisher pub(sinks, table, fcfg);
    pub.start();
    std::uint16_t retx_port = 30002, snap_port = 30003;
    if (cfg.contains("feed")) {
        if (cfg["feed"].contains("retransmit_port")) retx_port = cfg["feed"]["retransmit_port"].get<std::uint16_t>();
        if (cfg["feed"].contains("snapshot_port")) snap_port = cfg["feed"]["snapshot_port"].get<std::uint16_t>();
    }
    RetransmitServer retx(pub.store(), fcfg.session, retx_port);
    SnapshotService snap(ex, fcfg.session, snap_port);
    retx.start();
    snap.start();
    std::fprintf(stderr, "exchange_server: feed on %s:%u, retransmit tcp/%u, snapshot tcp/%u, %zu symbols, %zu shard(s), %.0f orders/s\n",
                 fcfg.group.c_str(), fcfg.port, retx_port, snap_port, ids.size(), shards, rate);

    // Adapters. Without an "ingress" list, the legacy "boe" block is used.
    std::vector<std::unique_ptr<IngressPlugin>> adapters;
    json ingress = json::array();
    if (cfg.contains("ingress") && cfg["ingress"].is_array()) {
        ingress = cfg["ingress"];
    } else {
        json b = json::object();
        if (cfg.contains("boe")) b = cfg["boe"];
        b["type"] = "boe";
        ingress.push_back(b);
    }
    for (const json &spec : ingress) {
        std::string type = spec.value("type", "boe");
        if (spec.value("enabled", true) == false)
            continue;
        std::unique_ptr<IngressPlugin> p;
        try {
            if (type == "boe")
                p = IngressPlugin::builtin("boe", BoeServer::entry());
            else if (type == "flowgen")
                p = IngressPlugin::builtin("flowgen", flowgen_ingress_entry());
            else if (type == "plugin")
                p = IngressPlugin::load(spec.at("path").get<std::string>());
            else {
                std::fprintf(stderr, "exchange_server: unknown ingress type %s\n", type.c_str());
                return 2;
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "exchange_server: %s\n", e.what());
            return 2;
        }
        json acfg = spec.contains("config") ? spec["config"] : spec;
        if (!p->init(core.api(), core.handle(), acfg.dump())) {
            std::fprintf(stderr, "exchange_server: ingress %s failed to init\n", p->name().c_str());
            return 2;
        }
        adapters.push_back(std::move(p));
    }
    for (auto &p : adapters)
        if (!p->start()) {
            std::fprintf(stderr, "exchange_server: ingress %s failed to start\n", p->name().c_str());
            return 2;
        }

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
            FlowGenerator gen(ex, shard_syms[sh], 42 + sh);
            std::uint64_t orders = 0, last_live = 0;
            const bool generate = rate > 0 && !shard_syms[sh].empty();
            while (!stop.load(std::memory_order_relaxed)) {
                bool worked = core.pump(sh) > 0;
                snap.serve_shard(sh);
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
        if (!quiet && now - last_report >= std::chrono::seconds(daemon ? 10 : 1)) {
            last_report = now;
            const auto &s = pub.stats();
            std::fprintf(stderr,
                         "[%6.1fs] orders=%llu live=%llu | feed msgs=%llu pkts=%llu hb=%llu "
                         "next_seq=%llu drops=%llu unpaired=%llu send_err=%llu errno=%d | "
                         "ingress sess=%llu in=%llu busy=%llu out=%llu drops=%llu\n",
                         elapsed_ns / 1e9, (unsigned long long)total_orders.load(),
                         (unsigned long long)total_live.load(),
                         (unsigned long long)s.messages.load(), (unsigned long long)s.packets.load(),
                         (unsigned long long)s.heartbeats.load(), (unsigned long long)s.next_seq.load(),
                         (unsigned long long)s.engine_drops.load(),
                         (unsigned long long)s.unpaired_executes.load(),
                         (unsigned long long)s.send_errors.load(), s.last_errno.load(),
                         (unsigned long long)core.stats().sessions_open.load(),
                         (unsigned long long)core.stats().submitted.load(),
                         (unsigned long long)core.stats().busy.load(),
                         (unsigned long long)core.stats().reports.load(),
                         (unsigned long long)core.stats().report_drops.load());
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
    for (auto &p : adapters)
        p->stop();
    adapters.clear();
    snap.stop();
    retx.stop();
    pub.stop();
    if (daemon)
        ::unlink(pidfile.c_str());
    const auto &s = pub.stats();
    std::fprintf(stderr, "exchange_server: done. orders=%llu msgs=%llu pkts=%llu drops=%llu\n",
                 (unsigned long long)orders, (unsigned long long)s.messages.load(),
                 (unsigned long long)s.packets.load(), (unsigned long long)s.engine_drops.load());
    return 0;
}
