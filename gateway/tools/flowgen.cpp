// flowgen: external synthetic order flow over BOE/TCP.
//
// Opens N sessions to the exchange's BOE gateway and drives the same order
// mix the in-process generator used (passive limits, cancels, modifies,
// crossing IOCs) at a target aggregate rate, tracking every order through
// its execution reports. Prints one stats line per second with round-trip
// latency percentiles.
//
//   flowgen [--host H] [--port P] [--sessions N] [--profile calm|busy|load | --rate R]
//           [--seconds S] [--symbols AAPL,GOOG,NVDA | --config configs/default.json] [--seed K] [--quiet]
// Without --symbols, the symbol list is read from --config (default:
// configs/default.json, then ../configs/default.json).
#include "protocol/boe.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

static volatile std::sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static std::uint64_t mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Totals {
    std::atomic<std::uint64_t> sent{0}, acked{0}, rejected{0}, busy{0}, unfilled{0}, execs{0},
        cancelled{0}, modified{0}, bytes_out{0}, bytes_in{0}, logouts{0}, outstanding{0};
    std::mutex mu;
    std::vector<std::uint64_t> rtt_ns; // acks since last report
    void rtt(std::uint64_t ns) {
        std::lock_guard<std::mutex> g(mu);
        if (rtt_ns.size() < 200000)
            rtt_ns.push_back(ns);
    }
};

struct Config {
    std::string host = "127.0.0.1";
    std::uint16_t port = 30000;
    int sessions = 4;
    double rate = -1; // aggregate orders/s; -1 = from --profile
    std::string profile = "calm"; // calm: 20/s per symbol, busy: 100/s per symbol, load: 20k total
    double seconds = 0;
    std::vector<std::string> symbols; // empty = from config
    std::string config;
    std::uint64_t seed = 1;
    bool quiet = false;
};

// One BOE session on one thread: paces new orders, reads reports.
class Session {
  public:
    Session(int idx, const Config &cfg, Totals &tot) : idx_(idx), cfg_(cfg), tot_(tot), rng_(cfg.seed * 7919 + idx) {
        mid_.assign(cfg.symbols.size(), 10000);
    }

    void run() {
        if (!connect_and_login())
            return;
        const double per_sec = cfg_.rate / cfg_.sessions;
        const double ns_per_order = per_sec > 0 ? 1e9 / per_sec : 0;
        const std::uint64_t start = mono_ns();
        std::uint64_t last_hb = start;
        std::uint64_t sent = 0;
        char rx[1 << 16];
        std::string rxbuf;
        while (!g_stop && !dead_) {
            const std::uint64_t now = mono_ns();
            // Send everything that is due, in one write, bounded burst.
            if (ns_per_order > 0) {
                int burst = 0;
                while (sent * ns_per_order <= double(now - start) && burst < 128) {
                    step();
                    ++sent;
                    ++burst;
                }
            }
            if (now - last_hb >= 1'000'000'000ULL) {
                frame(boe::MsgType::ClientHeartbeat, nullptr, 0, false);
                last_hb = now;
            }
            flush();
            // Wait for input until the next order is due.
            int timeout_ms = 0;
            if (ns_per_order > 0) {
                double next = start + (sent * ns_per_order);
                double wait = next - double(mono_ns());
                timeout_ms = wait > 0 ? std::max(1, int(wait / 1e6)) : 0;
                if (wait > 0 && wait < 1e6)
                    timeout_ms = 0;
            } else {
                timeout_ms = 100;
            }
            pollfd p{fd_, POLLIN, 0};
            int r = ::poll(&p, 1, timeout_ms);
            if (r > 0 && (p.revents & POLLIN)) {
                ssize_t n = ::recv(fd_, rx, sizeof(rx), 0);
                if (n <= 0) {
                    dead_ = true;
                    break;
                }
                tot_.bytes_in.fetch_add(n, std::memory_order_relaxed);
                rxbuf.append(rx, n);
                std::size_t off = 0;
                while (rxbuf.size() - off >= sizeof(boe::Header)) {
                    boe::Header h;
                    std::memcpy(&h, rxbuf.data() + off, sizeof(h));
                    std::size_t fl = 4 + h.message_length;
                    if (rxbuf.size() - off < fl)
                        break;
                    on_report(h, rxbuf.data() + off + sizeof(boe::Header), h.message_length - boe::kHeaderTail);
                    off += fl;
                }
                rxbuf.erase(0, off);
            } else if (r > 0 && (p.revents & (POLLERR | POLLHUP))) {
                dead_ = true;
            }
        }
        if (fd_ >= 0) {
            frame(boe::MsgType::LogoutRequest, nullptr, 0, false);
            flush();
            ::close(fd_);
        }
    }

  private:
    struct Live {
        std::uint64_t order_id;
        std::uint64_t px;
        std::uint32_t leaves;
        std::uint8_t side;
        std::size_t sym;
        std::size_t vec_pos;
    };

    bool connect_and_login() {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (::getaddrinfo(cfg_.host.c_str(), std::to_string(cfg_.port).c_str(), &hints, &res) != 0 || !res) {
            std::fprintf(stderr, "flowgen[%d]: cannot resolve %s\n", idx_, cfg_.host.c_str());
            return false;
        }
        fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd_ < 0 || ::connect(fd_, res->ai_addr, res->ai_addrlen) < 0) {
            std::fprintf(stderr, "flowgen[%d]: connect failed: %s\n", idx_, std::strerror(errno));
            ::freeaddrinfo(res);
            return false;
        }
        ::freeaddrinfo(res);
        int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        boe::LoginRequest l{};
        std::snprintf(l.session_sub_id, sizeof(l.session_sub_id) + 1, "%04d", idx_ % 10000);
        std::memcpy(l.username, "FLOW", 4);
        std::memset(l.password, ' ', sizeof(l.password));
        std::memcpy(l.password, "flow", 4);
        frame(boe::MsgType::LoginRequest, &l, sizeof(l), false);
        flush();
        // Wait for the LoginResponse before sending orders.
        char buf[256];
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, 3000) <= 0) {
            std::fprintf(stderr, "flowgen[%d]: no login response\n", idx_);
            return false;
        }
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < (ssize_t)(sizeof(boe::Header) + sizeof(boe::LoginResponse))) {
            std::fprintf(stderr, "flowgen[%d]: short login response\n", idx_);
            return false;
        }
        boe::LoginResponse r;
        std::memcpy(&r, buf + sizeof(boe::Header), sizeof(r));
        if (r.status != boe::login_status::Accepted) {
            std::fprintf(stderr, "flowgen[%d]: login rejected (%c)\n", idx_, r.status);
            return false;
        }
        return true;
    }

    void frame(boe::MsgType type, const void *body, std::uint16_t len, bool app) {
        boe::Header h{};
        h.message_length = static_cast<std::uint16_t>(boe::kHeaderTail + len);
        h.message_type = static_cast<std::uint8_t>(type);
        h.sequence_number = app ? out_seq_++ : 0;
        tx_.append(reinterpret_cast<const char *>(&h), sizeof(h));
        if (len)
            tx_.append(static_cast<const char *>(body), len);
    }

    void flush() {
        std::size_t off = 0;
        while (off < tx_.size()) {
            ssize_t n = ::send(fd_, tx_.data() + off, tx_.size() - off, 0);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                dead_ = true;
                break;
            }
            off += n;
        }
        tot_.bytes_out.fetch_add(off, std::memory_order_relaxed);
        tx_.clear();
    }

    std::string next_cl() {
        char b[21];
        std::snprintf(b, sizeof(b), "S%02d%017llu", idx_ % 100, (unsigned long long)cl_seq_++);
        return std::string(b, 20);
    }

    void send_new(std::size_t sym, std::uint8_t side, std::uint32_t qty, std::uint64_t px, std::uint8_t tif) {
        boe::NewOrder m{};
        std::string cl = next_cl();
        std::memcpy(m.cl_ord_id, cl.data(), 20);
        m.side = side;
        m.qty = qty;
        m.price = px;
        std::memset(m.symbol, ' ', sizeof(m.symbol));
        const std::string &s = cfg_.symbols[sym];
        std::memcpy(m.symbol, s.data(), std::min(s.size(), sizeof(m.symbol)));
        m.ord_type = boe::ord_type::Limit;
        m.tif = tif;
        frame(boe::MsgType::NewOrder, &m, sizeof(m), true);
        pending_[cl] = {mono_ns(), sym, side, px};
        tot_.sent.fetch_add(1, std::memory_order_relaxed);
        tot_.outstanding.fetch_add(1, std::memory_order_relaxed);
    }

    void step() {
        std::size_t si = rng_() % cfg_.symbols.size();
        std::uint64_t &mid = mid_[si];
        if (rng_() % 20 == 0)
            mid += (rng_() % 3) - 1;
        unsigned r = rng_() % 100;
        if (r < 55) {
            std::uint8_t side = (rng_() % 2) ? boe::side::Buy : boe::side::Sell;
            std::uint64_t off = 1 + rng_() % 15;
            std::uint64_t px = side == boe::side::Buy ? mid - off : mid + off;
            send_new(si, side, 1 + rng_() % 500, px, boe::tif::GTC);
        } else if (r < 80 && !live_vec_.empty()) {
            const std::string &cl = live_vec_[rng_() % live_vec_.size()];
            boe::CancelOrder c{};
            std::memcpy(c.orig_cl_ord_id, cl.data(), 20);
            frame(boe::MsgType::CancelOrder, &c, sizeof(c), true);
            tot_.sent.fetch_add(1, std::memory_order_relaxed);
        } else if (r < 90 && !live_vec_.empty()) {
            const std::string &cl = live_vec_[rng_() % live_vec_.size()];
            auto it = live_.find(cl);
            if (it != live_.end() && it->second.leaves > 1) {
                boe::ModifyOrder m{};
                std::memcpy(m.cl_ord_id, cl.data(), 20); // same id: shrink keeps priority
                std::memcpy(m.orig_cl_ord_id, cl.data(), 20);
                m.qty = 1 + rng_() % it->second.leaves;
                m.price = 0;
                frame(boe::MsgType::ModifyOrder, &m, sizeof(m), true);
                tot_.sent.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            std::uint8_t side = (rng_() % 2) ? boe::side::Buy : boe::side::Sell;
            std::uint64_t px = side == boe::side::Buy ? mid + 20 : (mid > 20 ? mid - 20 : 1);
            send_new(si, side, 1 + rng_() % 800, px, boe::tif::IOC);
        }
        if (live_vec_.size() > 5000) { // keep the book bounded per session
            const std::string cl = live_vec_[rng_() % live_vec_.size()];
            boe::CancelOrder c{};
            std::memcpy(c.orig_cl_ord_id, cl.data(), 20);
            frame(boe::MsgType::CancelOrder, &c, sizeof(c), true);
            tot_.sent.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void add_live(const std::string &cl, Live l) {
        l.vec_pos = live_vec_.size();
        live_vec_.push_back(cl);
        live_[cl] = l;
    }
    void remove_live(const std::string &cl) {
        auto it = live_.find(cl);
        if (it == live_.end())
            return;
        std::size_t pos = it->second.vec_pos;
        if (pos + 1 != live_vec_.size()) {
            live_vec_[pos] = live_vec_.back();
            live_[live_vec_[pos]].vec_pos = pos;
        }
        live_vec_.pop_back();
        live_.erase(it);
    }

    void on_report(const boe::Header &h, const char *body, std::uint16_t len) {
        using boe::MsgType;
        switch (static_cast<MsgType>(h.message_type)) {
        case MsgType::OrderAcknowledgment: {
            if (len != sizeof(boe::OrderAcknowledgment))
                return;
            boe::OrderAcknowledgment a;
            std::memcpy(&a, body, sizeof(a));
            std::string cl(a.cl_ord_id, 20);
            auto p = pending_.find(cl);
            if (p != pending_.end()) {
                tot_.rtt(mono_ns() - p->second.sent_ns);
                std::size_t sym = p->second.sym;
                pending_.erase(p);
                tot_.acked.fetch_add(1, std::memory_order_relaxed);
                if (a.leaves_qty > 0)
                    add_live(cl, Live{a.order_id, a.price, a.leaves_qty, a.side, sym, 0});
                else
                    tot_.outstanding.fetch_sub(1, std::memory_order_relaxed);
            }
            break;
        }
        case MsgType::OrderRejected: {
            if (len != sizeof(boe::OrderRejected))
                return;
            boe::OrderRejected r;
            std::memcpy(&r, body, sizeof(r));
            std::string cl(r.cl_ord_id, 20);
            if (pending_.erase(cl)) {
                tot_.outstanding.fetch_sub(1, std::memory_order_relaxed);
                if (r.reason == boe::reject_reason::Other)
                    tot_.busy.fetch_add(1, std::memory_order_relaxed);
                else if (r.reason == boe::reject_reason::NoLiquidity || r.reason == boe::reject_reason::FokUnfillable)
                    tot_.unfilled.fetch_add(1, std::memory_order_relaxed);
                else
                    tot_.rejected.fetch_add(1, std::memory_order_relaxed);
            } else if (r.reason != boe::reject_reason::UnknownOrder) {
                tot_.rejected.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
        case MsgType::OrderModified: {
            if (len != sizeof(boe::OrderModified))
                return;
            boe::OrderModified m;
            std::memcpy(&m, body, sizeof(m));
            auto it = live_.find(std::string(m.cl_ord_id, 20));
            if (it != live_.end())
                it->second.leaves = m.leaves_qty;
            tot_.modified.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MsgType::OrderCancelled: {
            if (len != sizeof(boe::OrderCancelled))
                return;
            boe::OrderCancelled c;
            std::memcpy(&c, body, sizeof(c));
            std::string cl(c.cl_ord_id, 20);
            if (live_.count(cl)) {
                remove_live(cl);
                tot_.outstanding.fetch_sub(1, std::memory_order_relaxed);
            } else if (pending_.erase(cl)) {
                tot_.outstanding.fetch_sub(1, std::memory_order_relaxed);
            }
            tot_.cancelled.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        case MsgType::OrderExecution: {
            if (len != sizeof(boe::OrderExecution))
                return;
            boe::OrderExecution x;
            std::memcpy(&x, body, sizeof(x));
            tot_.execs.fetch_add(1, std::memory_order_relaxed);
            std::string cl(x.cl_ord_id, 20);
            auto it = live_.find(cl);
            if (it != live_.end()) {
                mid_[it->second.sym] = x.last_price; // anchor the walk to real prints
                if (x.leaves_qty == 0) {
                    remove_live(cl);
                    tot_.outstanding.fetch_sub(1, std::memory_order_relaxed);
                } else {
                    it->second.leaves = x.leaves_qty;
                }
            }
            break;
        }
        case MsgType::Logout: {
            boe::Logout l;
            if (len == sizeof(l)) {
                std::memcpy(&l, body, sizeof(l));
                std::fprintf(stderr, "flowgen[%d]: logged out by server: %.60s\n", idx_, l.text);
            }
            tot_.logouts.fetch_add(1, std::memory_order_relaxed);
            dead_ = true;
            break;
        }
        default:
            break; // heartbeats, login response
        }
    }

    struct Pending {
        std::uint64_t sent_ns;
        std::size_t sym;
        std::uint8_t side;
        std::uint64_t px;
    };

    int idx_;
    const Config &cfg_;
    Totals &tot_;
    std::mt19937_64 rng_;
    int fd_ = -1;
    bool dead_ = false;
    std::uint32_t out_seq_ = 1;
    std::uint64_t cl_seq_ = 1;
    std::string tx_;
    std::vector<std::uint64_t> mid_;
    std::unordered_map<std::string, Pending> pending_;
    std::unordered_map<std::string, Live> live_;
    std::vector<std::string> live_vec_;
};

int main(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        auto val = [&](const char *name) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--host")) cfg.host = val("--host");
        else if (!std::strcmp(argv[i], "--port")) cfg.port = std::atoi(val("--port"));
        else if (!std::strcmp(argv[i], "--sessions")) cfg.sessions = std::max(1, std::atoi(val("--sessions")));
        else if (!std::strcmp(argv[i], "--rate")) cfg.rate = std::atof(val("--rate"));
        else if (!std::strcmp(argv[i], "--seconds")) cfg.seconds = std::atof(val("--seconds"));
        else if (!std::strcmp(argv[i], "--seed")) cfg.seed = std::strtoull(val("--seed"), nullptr, 10);
        else if (!std::strcmp(argv[i], "--quiet")) cfg.quiet = true;
        else if (!std::strcmp(argv[i], "--config")) cfg.config = val("--config");
        else if (!std::strcmp(argv[i], "--profile")) cfg.profile = val("--profile");
        else if (!std::strcmp(argv[i], "--symbols")) {
            cfg.symbols.clear();
            std::string s = val("--symbols");
            std::size_t p = 0;
            while (p <= s.size()) {
                std::size_t q = s.find(',', p);
                if (q == std::string::npos) q = s.size();
                if (q > p) cfg.symbols.push_back(s.substr(p, q - p));
                p = q + 1;
            }
        } else {
            std::fprintf(stderr, "usage: flowgen [--host H] [--port P] [--sessions N] [--profile calm|busy|load | --rate R] [--seconds S] [--symbols A,B | --config FILE] [--seed K] [--quiet]\n");
            return 2;
        }
    }
    if (cfg.symbols.empty()) {
        std::vector<std::string> candidates;
        if (!cfg.config.empty()) candidates.push_back(cfg.config);
        candidates.push_back("configs/default.json");
        candidates.push_back("../configs/default.json");
        for (const auto &path : candidates) {
            std::ifstream in(path);
            if (!in) continue;
            try {
                nlohmann::json j = nlohmann::json::parse(in);
                for (auto &[name, _] : j.at("symbols").items()) cfg.symbols.push_back(name);
                if (j.contains("boe") && j["boe"].contains("port") && cfg.port == 30000)
                    cfg.port = j["boe"]["port"].get<std::uint16_t>();
                if (j.contains("ingress"))
                    for (auto &a : j["ingress"])
                        if (a.value("type", "") == "boe" && a.contains("port") && cfg.port == 30000)
                            cfg.port = a["port"].get<std::uint16_t>();
                std::fprintf(stderr, "flowgen: %zu symbols from %s\n", cfg.symbols.size(), path.c_str());
                break;
            } catch (const std::exception &e) {
                std::fprintf(stderr, "flowgen: %s: %s\n", path.c_str(), e.what());
                return 2;
            }
        }
        if (cfg.symbols.empty()) {
            std::fprintf(stderr, "flowgen: no symbols; pass --symbols or --config\n");
            return 2;
        }
    }
    if (cfg.rate < 0) {
        const double n = static_cast<double>(cfg.symbols.size());
        if (cfg.profile == "busy") cfg.rate = 100.0 * n;
        else if (cfg.profile == "load") cfg.rate = 20000.0;
        else cfg.rate = 20.0 * n; // calm
    }
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    std::signal(SIGPIPE, SIG_IGN);

    Totals tot;
    std::vector<std::unique_ptr<Session>> sessions;
    std::vector<std::thread> threads;
    for (int i = 0; i < cfg.sessions; ++i) {
        sessions.push_back(std::make_unique<Session>(i, cfg, tot));
        threads.emplace_back([&, i] { sessions[i]->run(); });
    }
    std::fprintf(stderr, "flowgen: %d session(s) to %s:%u, %.0f orders/s total over %zu symbol(s)\n",
                 cfg.sessions, cfg.host.c_str(), cfg.port, cfg.rate, cfg.symbols.size());

    const auto start = std::chrono::steady_clock::now();
    std::uint64_t last_sent = 0;
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::vector<std::uint64_t> rtt;
        {
            std::lock_guard<std::mutex> g(tot.mu);
            rtt.swap(tot.rtt_ns);
        }
        std::uint64_t p50 = 0, p99 = 0, mx = 0;
        if (!rtt.empty()) {
            std::sort(rtt.begin(), rtt.end());
            p50 = rtt[rtt.size() / 2];
            p99 = rtt[(rtt.size() * 99) / 100];
            mx = rtt.back();
        }
        std::uint64_t sent = tot.sent.load();
        if (!cfg.quiet)
            std::fprintf(stderr,
                         "[%6.1fs] sent=%llu (%llu/s) acked=%llu rej=%llu busy=%llu noliq=%llu exec=%llu cxl=%llu mod=%llu "
                         "live=%llu rtt p50=%lluµs p99=%lluµs max=%lluµs out=%.1fMB in=%.1fMB%s\n",
                         el, (unsigned long long)sent, (unsigned long long)(sent - last_sent),
                         (unsigned long long)tot.acked.load(), (unsigned long long)tot.rejected.load(),
                         (unsigned long long)tot.busy.load(), (unsigned long long)tot.unfilled.load(),
                         (unsigned long long)tot.execs.load(),
                         (unsigned long long)tot.cancelled.load(), (unsigned long long)tot.modified.load(),
                         (unsigned long long)tot.outstanding.load(), (unsigned long long)(p50 / 1000),
                         (unsigned long long)(p99 / 1000), (unsigned long long)(mx / 1000),
                         tot.bytes_out.load() / 1e6, tot.bytes_in.load() / 1e6,
                         tot.logouts.load() ? " LOGOUT" : "");
        last_sent = sent;
        if (cfg.seconds > 0 && el >= cfg.seconds)
            break;
        if (tot.logouts.load() >= (std::uint64_t)cfg.sessions)
            break;
    }
    g_stop = 1;
    for (auto &t : threads)
        t.join();
    std::fprintf(stderr, "flowgen: done. sent=%llu acked=%llu rejected=%llu busy=%llu execs=%llu\n",
                 (unsigned long long)tot.sent.load(), (unsigned long long)tot.acked.load(),
                 (unsigned long long)tot.rejected.load(), (unsigned long long)tot.busy.load(),
                 (unsigned long long)tot.execs.load());
    return 0;
}
