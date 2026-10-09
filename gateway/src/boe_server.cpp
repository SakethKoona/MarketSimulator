#include "boe_server.hpp"
#include "common.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::string trim_field(const char *p, std::size_t n) {
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\0'))
        --n;
    return std::string(p, n);
}

std::string key_of(const char *cl) { return std::string(cl, boe::kClOrdIdLen); }

void set_nonblock(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

std::uint8_t reject_reason_for(StatusCode s) {
    switch (s) {
    case StatusCode::SymbolNotFound: return boe::reject_reason::UnknownSymbol;
    case StatusCode::InvalidQuantity: return boe::reject_reason::BadQty;
    case StatusCode::InvalidPrice: return boe::reject_reason::BadPrice;
    case StatusCode::FOKFailed: return boe::reject_reason::FokUnfillable;
    case StatusCode::DuplicateOrder: return boe::reject_reason::DuplicateClOrdId;
    case StatusCode::OrderNotFound: return boe::reject_reason::UnknownOrder;
    default: return boe::reject_reason::Other;
    }
}

const char *reject_text_for(StatusCode s) {
    switch (s) {
    case StatusCode::SymbolNotFound: return "unknown symbol";
    case StatusCode::InvalidQuantity: return "invalid quantity";
    case StatusCode::InvalidPrice: return "invalid price";
    case StatusCode::FOKFailed: return "fill-or-kill could not be filled";
    case StatusCode::DuplicateOrder: return "duplicate order";
    case StatusCode::OrderNotFound: return "unknown order";
    case StatusCode::NotEnoughLiquidity: return "not enough liquidity";
    default: return "rejected";
    }
}

} // namespace

BoeServer::BoeServer(BoeConfig cfg, std::vector<SpscRing<InboundCommand> *> commands,
                     std::vector<SpscRing<OrderReport> *> reports, SymbolMap symbols,
                     std::vector<std::uint8_t> symbol_shard)
    : cfg_(std::move(cfg)), commands_(std::move(commands)), reports_(std::move(reports)),
      symbols_(std::move(symbols)), symbol_shard_(std::move(symbol_shard)) {
    if (commands_.empty() || commands_.size() != reports_.size())
        throw std::runtime_error("BoeServer: need one command and one report ring per shard");
    std::size_t n = 0;
    for (auto &[name, id] : symbols_)
        if (id + 1 > n)
            n = id + 1;
    tickers_.assign(n, std::string(boe::kSymbolLen, ' '));
    for (auto &[name, id] : symbols_) {
        std::string t = name.substr(0, boe::kSymbolLen);
        t.resize(boe::kSymbolLen, ' ');
        tickers_[id] = t;
    }
    sessions_.resize(cfg_.max_sessions);
    for (std::uint32_t i = 0; i < cfg_.max_sessions; ++i)
        free_slots_.push_back(cfg_.max_sessions - 1 - i);
}

BoeServer::~BoeServer() { stop(); }

std::uint64_t BoeServer::now_ns() const { return get_monotonic_ns(); }

void BoeServer::start() {
    if (running_.exchange(true))
        return;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.bind.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("bad bind address: " + cfg_.bind);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0)
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    if (::listen(listen_fd_, 64) < 0)
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    set_nonblock(listen_fd_);
    thread_ = std::thread([this] { run(); });
}

void BoeServer::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    for (std::uint32_t i = 0; i < sessions_.size(); ++i)
        if (sessions_[i].fd >= 0)
            close_session(i);
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool BoeServer::session_alive(std::uint32_t si, std::uint32_t gen) const {
    return si < sessions_.size() && sessions_[si].fd >= 0 && sessions_[si].gen == gen;
}

void BoeServer::run() {
    std::vector<pollfd> pfds;
    const std::uint64_t hb_ns = std::uint64_t(cfg_.heartbeat_ms) * 1'000'000;
    const std::uint64_t to_ns = std::uint64_t(cfg_.timeout_ms) * 1'000'000;

    while (running_.load(std::memory_order_relaxed)) {
        pfds.clear();
        pfds.push_back({listen_fd_, POLLIN, 0});
        for (auto &s : sessions_) {
            if (s.fd < 0)
                continue;
            short ev = POLLIN;
            if (!s.tx.empty())
                ev |= POLLOUT;
            pfds.push_back({s.fd, ev, 0});
        }
        int n = ::poll(pfds.data(), pfds.size(), static_cast<int>(cfg_.poll_ms));
        if (n > 0) {
            if (pfds[0].revents & POLLIN)
                accept_new();
            std::size_t k = 1;
            for (std::uint32_t si = 0; si < sessions_.size(); ++si) {
                if (sessions_[si].fd < 0)
                    continue;
                if (k >= pfds.size())
                    break;
                pollfd &p = pfds[k++];
                if (p.fd != sessions_[si].fd)
                    continue;
                if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    close_session(si);
                    continue;
                }
                if (p.revents & POLLIN)
                    on_readable(si);
                if (sessions_[si].fd >= 0 && (p.revents & POLLOUT))
                    on_writable(si);
            }
        }

        drain_reports();

        const std::uint64_t now = now_ns();
        for (std::uint32_t si = 0; si < sessions_.size(); ++si) {
            Session &s = sessions_[si];
            if (s.fd < 0)
                continue;
            if (now - s.last_rx_ns > to_ns) {
                close_session(si);
                continue;
            }
            if (now - s.last_tx_ns > hb_ns)
                send_raw(si, boe::MsgType::ServerHeartbeat, nullptr, 0, false);
            if (!s.tx.empty())
                on_writable(si);
        }
    }
}

void BoeServer::accept_new() {
    for (;;) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
            return;
        if (free_slots_.empty()) {
            ::close(fd);
            continue;
        }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        set_nonblock(fd);
        std::uint32_t si = free_slots_.back();
        free_slots_.pop_back();
        Session &s = sessions_[si];
        s.fd = fd;
        s.gen++;
        s.logged_in = false;
        s.next_in_seq = 1;
        s.next_out_seq = 1;
        s.last_rx_ns = s.last_tx_ns = now_ns();
        s.rx.clear();
        s.tx.clear();
        s.clords.clear();
        stats_.sessions.fetch_add(1, std::memory_order_relaxed);
    }
}

void BoeServer::close_session(std::uint32_t si) {
    Session &s = sessions_[si];
    if (s.fd < 0)
        return;
    ::close(s.fd);
    s.fd = -1;
    s.logged_in = false;
    s.rx.clear();
    s.tx.clear();
    s.clords.clear();
    free_slots_.push_back(si);
    // live_/pending_ entries keyed to this (si, gen) go stale and are
    // ignored by session_alive(); v2 may add cancel-on-disconnect here.
}

void BoeServer::on_readable(std::uint32_t si) {
    Session &s = sessions_[si];
    char buf[16 * 1024];
    for (;;) {
        ssize_t n = ::recv(s.fd, buf, sizeof(buf), 0);
        if (n == 0) {
            close_session(si);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            close_session(si);
            return;
        }
        s.rx.insert(s.rx.end(), buf, buf + n);
        s.last_rx_ns = now_ns();
        if (s.rx.size() > 1 << 20) { // a client that never frames correctly
            close_session(si);
            return;
        }
    }

    std::size_t off = 0;
    while (s.rx.size() - off >= sizeof(boe::Header)) {
        boe::Header h;
        std::memcpy(&h, s.rx.data() + off, sizeof(h));
        if (h.start_of_message != boe::kStartOfMessage) {
            close_session(si);
            return;
        }
        if (h.message_length < boe::kHeaderTail) {
            close_session(si);
            return;
        }
        std::size_t frame = 4 + h.message_length;
        if (s.rx.size() - off < frame)
            break;
        std::uint16_t body_len = h.message_length - boe::kHeaderTail;
        handle_frame(si, h, s.rx.data() + off + sizeof(boe::Header), body_len);
        if (s.fd < 0)
            return; // closed during handling
        off += frame;
    }
    if (off)
        s.rx.erase(s.rx.begin(), s.rx.begin() + off);
}

void BoeServer::on_writable(std::uint32_t si) {
    Session &s = sessions_[si];
    while (!s.tx.empty()) {
        ssize_t n = ::send(s.fd, s.tx.data(), s.tx.size(), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            close_session(si);
            return;
        }
        s.tx.erase(s.tx.begin(), s.tx.begin() + n);
    }
}

void BoeServer::send_raw(std::uint32_t si, boe::MsgType type, const void *body,
                         std::uint16_t len, bool app_msg) {
    Session &s = sessions_[si];
    boe::Header h{};
    h.message_length = static_cast<std::uint16_t>(boe::kHeaderTail + len);
    h.message_type = static_cast<std::uint8_t>(type);
    h.sequence_number = app_msg ? s.next_out_seq++ : 0;
    const char *hp = reinterpret_cast<const char *>(&h);
    s.tx.insert(s.tx.end(), hp, hp + sizeof(h));
    if (len)
        s.tx.insert(s.tx.end(), static_cast<const char *>(body),
                    static_cast<const char *>(body) + len);
    s.last_tx_ns = now_ns();
    if (app_msg)
        stats_.reports_out.fetch_add(1, std::memory_order_relaxed);
    on_writable(si);
}

template <typename Body>
void BoeServer::send(std::uint32_t si, boe::MsgType type, const Body &b, bool app_msg) {
    send_raw(si, type, &b, static_cast<std::uint16_t>(sizeof(Body)), app_msg);
}

void BoeServer::reject(std::uint32_t si, const ClOrdId &cl, std::uint8_t reason,
                       const char *text) {
    boe::OrderRejected r{};
    r.ts_ns = get_current_timestamp();
    std::memcpy(r.cl_ord_id, cl.v, boe::kClOrdIdLen);
    r.reason = reason;
    std::memset(r.text, ' ', sizeof(r.text));
    std::size_t n = std::strlen(text);
    std::memcpy(r.text, text, n < sizeof(r.text) ? n : sizeof(r.text));
    send(si, boe::MsgType::OrderRejected, r);
    stats_.rejects.fetch_add(1, std::memory_order_relaxed);
}

void BoeServer::handle_frame(std::uint32_t si, const boe::Header &h, const char *body,
                             std::uint16_t body_len) {
    Session &s = sessions_[si];
    using boe::MsgType;
    const auto type = static_cast<MsgType>(h.message_type);

    // Session-layer messages carry seq 0 and need no login.
    switch (type) {
    case MsgType::LoginRequest: {
        if (body_len != sizeof(boe::LoginRequest)) {
            close_session(si);
            return;
        }
        boe::LoginRequest m;
        std::memcpy(&m, body, sizeof(m));
        boe::LoginResponse r{};
        std::memset(r.text, ' ', sizeof(r.text));
        if (s.logged_in) {
            r.status = boe::login_status::Duplicate;
            std::memcpy(r.text, "already logged in", 17);
        } else {
            // v1 accepts any credentials; the session sub id is recorded.
            s.logged_in = true;
            s.sub_id = trim_field(m.session_sub_id, sizeof(m.session_sub_id));
            r.status = boe::login_status::Accepted;
            std::memcpy(r.text, "welcome", 7);
            stats_.logins.fetch_add(1, std::memory_order_relaxed);
        }
        r.last_received_seq = s.next_in_seq - 1;
        send(si, MsgType::LoginResponse, r, false);
        return;
    }
    case MsgType::LogoutRequest: {
        boe::Logout r{};
        r.reason = 'U';
        std::memset(r.text, ' ', sizeof(r.text));
        std::memcpy(r.text, "user requested", 14);
        send(si, MsgType::Logout, r, false);
        close_session(si);
        return;
    }
    case MsgType::ClientHeartbeat:
        return;
    default:
        break;
    }

    // Application messages: must be logged in, must be in sequence.
    if (!s.logged_in) {
        ClOrdId cl{};
        std::memset(cl.v, ' ', sizeof(cl.v));
        if (body_len >= boe::kClOrdIdLen)
            std::memcpy(cl.v, body, boe::kClOrdIdLen);
        reject(si, cl, boe::reject_reason::NotLoggedIn, "not logged in");
        return;
    }
    if (h.sequence_number != s.next_in_seq) {
        boe::Logout r{};
        r.reason = 'S';
        std::memset(r.text, ' ', sizeof(r.text));
        std::snprintf(r.text, sizeof(r.text), "sequence error: expected %u got %u",
                      s.next_in_seq, h.sequence_number);
        for (char &c : r.text)
            if (c == '\0')
                c = ' ';
        send(si, MsgType::Logout, r, false);
        close_session(si);
        return;
    }
    s.next_in_seq++;

    switch (type) {
    case MsgType::NewOrder: {
        if (body_len != sizeof(boe::NewOrder)) {
            close_session(si);
            return;
        }
        boe::NewOrder m;
        std::memcpy(&m, body, sizeof(m));
        handle_new(si, m);
        return;
    }
    case MsgType::CancelOrder: {
        if (body_len != sizeof(boe::CancelOrder)) {
            close_session(si);
            return;
        }
        boe::CancelOrder m;
        std::memcpy(&m, body, sizeof(m));
        handle_cancel(si, m);
        return;
    }
    case MsgType::ModifyOrder: {
        if (body_len != sizeof(boe::ModifyOrder)) {
            close_session(si);
            return;
        }
        boe::ModifyOrder m;
        std::memcpy(&m, body, sizeof(m));
        handle_modify(si, m);
        return;
    }
    default:
        return; // unknown but well-framed: skip
    }
}

void BoeServer::handle_new(std::uint32_t si, const boe::NewOrder &m) {
    Session &s = sessions_[si];
    stats_.orders_in.fetch_add(1, std::memory_order_relaxed);
    ClOrdId cl;
    std::memcpy(cl.v, m.cl_ord_id, boe::kClOrdIdLen);

    auto sym = symbols_.find(trim_field(m.symbol, boe::kSymbolLen));
    if (sym == symbols_.end())
        return reject(si, cl, boe::reject_reason::UnknownSymbol, "unknown symbol");
    if (m.qty == 0)
        return reject(si, cl, boe::reject_reason::BadQty, "quantity must be > 0");
    if (m.side != boe::side::Buy && m.side != boe::side::Sell)
        return reject(si, cl, boe::reject_reason::Other, "bad side");
    if (m.ord_type != boe::ord_type::Market && m.ord_type != boe::ord_type::Limit)
        return reject(si, cl, boe::reject_reason::Other, "bad order type");
    if (m.ord_type == boe::ord_type::Limit && m.price == 0)
        return reject(si, cl, boe::reject_reason::BadPrice, "limit price must be > 0");
    if (m.tif != boe::tif::GTC && m.tif != boe::tif::IOC && m.tif != boe::tif::FOK)
        return reject(si, cl, boe::reject_reason::Other, "bad time in force");
    if (m.ord_type == boe::ord_type::Market && m.tif == boe::tif::GTC)
        return reject(si, cl, boe::reject_reason::Other, "market orders must be IOC or FOK");
    if (s.clords.count(key_of(m.cl_ord_id)))
        return reject(si, cl, boe::reject_reason::DuplicateClOrdId, "duplicate cl_ord_id");

    InboundCommand c{};
    c.request_id = next_request_++;
    c.type = CommandType::NewOrder;
    c.side = m.side == boe::side::Buy ? Side::Buy : Side::Sell;
    c.ord_type = m.ord_type == boe::ord_type::Market ? OrderType::MARKET : OrderType::LIMIT;
    c.tif = m.tif == boe::tif::IOC ? TypeInForce::IOC
            : m.tif == boe::tif::FOK ? TypeInForce::FOK
                                     : TypeInForce::GTC;
    c.symbol_id = static_cast<std::uint32_t>(sym->second);
    c.qty = m.qty;
    c.price = m.price;
    c.ts_ns = now_ns();
    std::size_t shard = c.symbol_id < symbol_shard_.size() ? symbol_shard_[c.symbol_id] : 0;
    if (!push_command(c, shard))
        return reject(si, cl, boe::reject_reason::Other, "exchange busy");
    // Reserve the cl_ord_id now so a duplicate sent before the ack is caught.
    s.clords[key_of(m.cl_ord_id)] = 0;
    pending_[c.request_id] = Pending{si, s.gen, cl, cl, CommandType::NewOrder, m.price};
}

void BoeServer::handle_cancel(std::uint32_t si, const boe::CancelOrder &m) {
    Session &s = sessions_[si];
    stats_.cancels_in.fetch_add(1, std::memory_order_relaxed);
    ClOrdId cl;
    std::memcpy(cl.v, m.orig_cl_ord_id, boe::kClOrdIdLen);
    auto it = s.clords.find(key_of(m.orig_cl_ord_id));
    if (it == s.clords.end() || it->second == 0)
        return reject(si, cl, boe::reject_reason::UnknownOrder, "unknown order");

    InboundCommand c{};
    c.request_id = next_request_++;
    c.type = CommandType::Cancel;
    c.order_id = it->second;
    c.ts_ns = now_ns();
    if (!push_command(c, shard_of(c.order_id)))
        return reject(si, cl, boe::reject_reason::Other, "exchange busy");
    pending_[c.request_id] = Pending{si, s.gen, cl, cl, CommandType::Cancel, 0};
}

void BoeServer::handle_modify(std::uint32_t si, const boe::ModifyOrder &m) {
    Session &s = sessions_[si];
    stats_.modifies_in.fetch_add(1, std::memory_order_relaxed);
    ClOrdId cl, orig;
    std::memcpy(cl.v, m.cl_ord_id, boe::kClOrdIdLen);
    std::memcpy(orig.v, m.orig_cl_ord_id, boe::kClOrdIdLen);
    auto it = s.clords.find(key_of(m.orig_cl_ord_id));
    if (it == s.clords.end() || it->second == 0)
        return reject(si, cl, boe::reject_reason::UnknownOrder, "unknown order");
    if (m.qty == 0)
        return reject(si, cl, boe::reject_reason::BadQty, "quantity must be > 0; use cancel");
    if (key_of(m.cl_ord_id) != key_of(m.orig_cl_ord_id) && s.clords.count(key_of(m.cl_ord_id)))
        return reject(si, cl, boe::reject_reason::DuplicateClOrdId, "duplicate cl_ord_id");

    InboundCommand c{};
    c.request_id = next_request_++;
    c.type = CommandType::Modify;
    c.order_id = it->second;
    c.qty = m.qty;
    c.price = m.price;
    c.ts_ns = now_ns();
    if (!push_command(c, shard_of(c.order_id)))
        return reject(si, cl, boe::reject_reason::Other, "exchange busy");
    pending_[c.request_id] = Pending{si, s.gen, cl, orig, CommandType::Modify, m.price};
}

bool BoeServer::push_command(const InboundCommand &c, std::size_t shard) {
    if (shard >= commands_.size() || !commands_[shard]->try_push(c)) {
        stats_.ring_full.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void BoeServer::drain_reports() {
    // Round-robin over shards, bounded per shard per pass so one busy shard
    // cannot starve the others.
    OrderReport r;
    bool any = true;
    while (any) {
        any = false;
        for (auto *ring : reports_) {
            for (int i = 0; i < 64 && ring->try_pop(r); ++i) {
                on_report(r);
                any = true;
            }
        }
    }
}

void BoeServer::on_report(const OrderReport &r) {
    const std::uint64_t ts = get_current_timestamp();

    if (r.kind == ReportKind::Execution) {
        auto it = live_.find(r.order_id);
        if (it == live_.end())
            return; // not a session's order (synthetic flow)
        Live &o = it->second;
        o.leaves = r.last_qty >= o.leaves ? 0 : o.leaves - r.last_qty;
        if (session_alive(o.sess, o.gen)) {
            boe::OrderExecution x{};
            x.ts_ns = ts;
            std::memcpy(x.cl_ord_id, o.cl.v, boe::kClOrdIdLen);
            x.order_id = r.order_id;
            x.match_id = r.match_id;
            x.last_qty = r.last_qty;
            x.last_price = r.price;
            x.leaves_qty = o.leaves;
            x.side = o.side == Side::Buy ? boe::side::Buy : boe::side::Sell;
            send(o.sess, boe::MsgType::OrderExecution, x);
        }
        if (o.leaves == 0) {
            if (session_alive(o.sess, o.gen))
                sessions_[o.sess].clords.erase(key_of(o.cl.v));
            live_.erase(it);
        }
        return;
    }

    // Everything else answers a request.
    auto pit = pending_.find(r.request_id);
    if (pit == pending_.end()) {
        // Unsolicited Cancelled (e.g. IOC remainder arrives after its
        // Accepted removed the pending entry): route by order id.
        if (r.kind == ReportKind::Cancelled) {
            auto it = live_.find(r.order_id);
            if (it == live_.end())
                return;
            Live &o = it->second;
            if (session_alive(o.sess, o.gen)) {
                boe::OrderCancelled c{};
                c.ts_ns = ts;
                std::memcpy(c.cl_ord_id, o.cl.v, boe::kClOrdIdLen);
                c.order_id = r.order_id;
                c.reason = r.status == StatusCode::FOKFailed ? boe::cancel_reason::FokFailed
                                                              : boe::cancel_reason::IocRemainder;
                send(o.sess, boe::MsgType::OrderCancelled, c);
                sessions_[o.sess].clords.erase(key_of(o.cl.v));
            }
            live_.erase(it);
        }
        return;
    }
    Pending p = pit->second;
    pending_.erase(pit);
    const bool alive = session_alive(p.sess, p.gen);

    switch (r.kind) {
    case ReportKind::Accepted: {
        Live o{p.sess, p.gen, p.cl, r.side, r.symbol_id, r.price, r.qty};
        live_[r.order_id] = o;
        if (alive) {
            sessions_[p.sess].clords[key_of(p.cl.v)] = r.order_id;
            boe::OrderAcknowledgment a{};
            a.ts_ns = ts;
            std::memcpy(a.cl_ord_id, p.cl.v, boe::kClOrdIdLen);
            a.order_id = r.order_id;
            std::memcpy(a.symbol, tickers_[r.symbol_id].data(), boe::kSymbolLen);
            a.side = r.side == Side::Buy ? boe::side::Buy : boe::side::Sell;
            a.qty = r.qty;
            a.price = r.price;
            a.leaves_qty = r.leaves_qty;
            send(p.sess, boe::MsgType::OrderAcknowledgment, a);
        }
        break;
    }
    case ReportKind::Rejected: {
        if (alive) {
            if (p.type == CommandType::NewOrder)
                sessions_[p.sess].clords.erase(key_of(p.cl.v));
            reject(p.sess, p.cl, reject_reason_for(r.status), reject_text_for(r.status));
        }
        break;
    }
    case ReportKind::Cancelled: {
        auto it = live_.find(r.order_id);
        if (alive) {
            boe::OrderCancelled c{};
            c.ts_ns = ts;
            std::memcpy(c.cl_ord_id, p.cl.v, boe::kClOrdIdLen);
            c.order_id = r.order_id;
            c.reason = p.type == CommandType::Cancel || p.type == CommandType::Modify
                           ? boe::cancel_reason::User
                       : r.status == StatusCode::FOKFailed ? boe::cancel_reason::FokFailed
                                                            : boe::cancel_reason::IocRemainder;
            send(p.sess, boe::MsgType::OrderCancelled, c);
            sessions_[p.sess].clords.erase(key_of(p.cl.v));
            if (it != live_.end())
                sessions_[p.sess].clords.erase(key_of(it->second.cl.v));
        }
        if (it != live_.end())
            live_.erase(it);
        break;
    }
    case ReportKind::Modified: {
        auto it = live_.find(r.order_id);
        if (it != live_.end()) {
            Live &o = it->second;
            o.cl = p.cl;
            o.leaves = r.qty;
            if (r.price)
                o.price = r.price;
            if (alive) {
                Session &s = sessions_[p.sess];
                s.clords.erase(key_of(p.orig.v));
                s.clords[key_of(p.cl.v)] = r.order_id;
                boe::OrderModified m{};
                m.ts_ns = ts;
                std::memcpy(m.cl_ord_id, p.cl.v, boe::kClOrdIdLen);
                m.order_id = r.order_id;
                m.qty = r.qty;
                m.price = o.price;
                m.leaves_qty = r.qty;
                send(p.sess, boe::MsgType::OrderModified, m);
            }
        }
        break;
    }
    case ReportKind::Execution:
        break; // handled above
    }
}
