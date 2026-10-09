#include "remote_session.hpp"
#include "protocol/boe.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace {

std::uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string trim_field(const char *p, std::size_t n) {
    std::string s(p, n);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
        s.pop_back();
    return s;
}

void put_field(char *dst, std::size_t n, const std::string &s) {
    std::memset(dst, ' ', n);
    std::memcpy(dst, s.data(), std::min(n, s.size()));
}

StatusCode status_from_reject(std::uint8_t reason) {
    switch (reason) {
    case boe::reject_reason::UnknownSymbol: return StatusCode::SymbolNotFound;
    case boe::reject_reason::BadQty: return StatusCode::InvalidQuantity;
    case boe::reject_reason::BadPrice: return StatusCode::InvalidPrice;
    case boe::reject_reason::FokUnfillable: return StatusCode::FOKFailed;
    case boe::reject_reason::DuplicateClOrdId: return StatusCode::DuplicateOrder;
    case boe::reject_reason::UnknownOrder: return StatusCode::OrderNotFound;
    default: return StatusCode::Failed;
    }
}

} // namespace

RemoteSession::RemoteSession(const std::string &host, std::uint16_t port,
                             const std::string &user,
                             const std::string &password)
    : host_(host), port_(port) {
    // Resolve and connect
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        throw std::runtime_error("cannot resolve " + host);
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0 || ::connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        throw std::runtime_error("cannot connect to " + host + ":" +
                                 std::to_string(port) + " (" + std::strerror(errno) + ")");
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    running_ = true;
    connected_ = true;
    reader_ = std::thread([this] { reader_loop(); });

    // Login (session message, seq 0)
    boe::LoginRequest lr{};
    put_field(lr.session_sub_id, sizeof lr.session_sub_id, "0001");
    put_field(lr.username, sizeof lr.username, user);
    put_field(lr.password, sizeof lr.password, password);
    send_frame(static_cast<std::uint8_t>(boe::MsgType::LoginRequest), &lr, sizeof lr, false);

    Report r;
    if (!await("", r, 3000) || r.type != static_cast<std::uint8_t>(boe::MsgType::LoginResponse))
        throw std::runtime_error("no login response from " + host);
    if (r.reason != boe::login_status::Accepted)
        throw std::runtime_error("login rejected: " + r.text);
}

RemoteSession::~RemoteSession() {
    if (connected_) {
        try {
            send_frame(static_cast<std::uint8_t>(boe::MsgType::LogoutRequest), nullptr, 0, false);
        } catch (...) {
        }
    }
    running_ = false;
    if (fd_ >= 0)
        ::shutdown(fd_, SHUT_RDWR);
    if (reader_.joinable())
        reader_.join();
    if (fd_ >= 0)
        ::close(fd_);
}

std::string RemoteSession::Describe() const {
    return "BOE session to " + host_ + ":" + std::to_string(port_);
}

/* ---------------- wire ---------------- */

void RemoteSession::send_frame(std::uint8_t type, const void *body,
                               std::size_t len, bool app_msg) {
    std::lock_guard<std::mutex> lk(tx_mu_);
    char buf[sizeof(boe::Header) + boe::kMaxBodySize];
    boe::Header h{};
    h.start_of_message = boe::kStartOfMessage;
    h.message_length = static_cast<std::uint16_t>(boe::kHeaderTail + len);
    h.message_type = type;
    h.matching_unit = 0;
    h.sequence_number = app_msg ? out_seq_++ : 0;
    std::memcpy(buf, &h, sizeof h);
    if (len)
        std::memcpy(buf + sizeof h, body, len);
    std::size_t total = sizeof h + len, off = 0;
    while (off < total) {
        ssize_t n = ::send(fd_, buf + off, total - off, 0);
        if (n <= 0) {
            connected_ = false;
            throw std::runtime_error("connection lost");
        }
        off += static_cast<std::size_t>(n);
    }
    last_tx_ns_ = now_ns();
}

void RemoteSession::send_heartbeat() {
    try {
        send_frame(static_cast<std::uint8_t>(boe::MsgType::ClientHeartbeat), nullptr, 0, false);
    } catch (...) {
    }
}

void RemoteSession::reader_loop() {
    std::vector<char> rx;
    char chunk[4096];
    while (running_) {
        pollfd p{fd_, POLLIN, 0};
        int rc = ::poll(&p, 1, 200);
        if (rc < 0)
            break;
        if (rc == 0) {
            if (now_ns() - last_tx_ns_ > 1'000'000'000ULL)
                send_heartbeat();
            continue;
        }
        ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
        if (n <= 0)
            break;
        rx.insert(rx.end(), chunk, chunk + n);

        std::size_t off = 0;
        while (rx.size() - off >= 4) {
            std::uint16_t som, len;
            std::memcpy(&som, rx.data() + off, 2);
            std::memcpy(&len, rx.data() + off + 2, 2);
            if (som != boe::kStartOfMessage) {
                running_ = false;
                break;
            }
            if (rx.size() - off < 4u + len)
                break;
            handle_frame(rx.data() + off, 4u + len);
            off += 4u + len;
        }
        rx.erase(rx.begin(), rx.begin() + off);
    }
    connected_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (logout_text_.empty())
            logout_text_ = "connection closed";
    }
    cv_.notify_all();
}

void RemoteSession::handle_frame(const char *buf, std::size_t len) {
    using boe::MsgType;
    boe::Header h;
    std::memcpy(&h, buf, sizeof h);
    const char *body = buf + sizeof h;
    std::size_t blen = len - sizeof h;
    Report r;
    r.type = h.message_type;

    auto cl = [&](const char *p) { return std::string(p, boe::kClOrdIdLen); };

    switch (static_cast<MsgType>(h.message_type)) {
    case MsgType::ServerHeartbeat:
        return;
    case MsgType::LoginResponse: {
        if (blen < sizeof(boe::LoginResponse)) return;
        boe::LoginResponse m; std::memcpy(&m, body, sizeof m);
        r.reason = m.status;
        r.text = trim_field(m.text, sizeof m.text);
        break;
    }
    case MsgType::Logout: {
        if (blen < sizeof(boe::Logout)) return;
        boe::Logout m; std::memcpy(&m, body, sizeof m);
        std::lock_guard<std::mutex> lk(mu_);
        logout_text_ = "logged out by server: " + trim_field(m.text, sizeof m.text);
        notices_.push_back(logout_text_);
        cv_.notify_all();
        return;
    }
    case MsgType::OrderAcknowledgment: {
        if (blen < sizeof(boe::OrderAcknowledgment)) return;
        boe::OrderAcknowledgment m; std::memcpy(&m, body, sizeof m);
        r.cl_ord_id = cl(m.cl_ord_id); r.order_id = m.order_id; r.qty = m.qty;
        r.price = m.price; r.leaves = m.leaves_qty;
        break;
    }
    case MsgType::OrderRejected: {
        if (blen < sizeof(boe::OrderRejected)) return;
        boe::OrderRejected m; std::memcpy(&m, body, sizeof m);
        r.cl_ord_id = cl(m.cl_ord_id); r.reason = m.reason;
        r.text = trim_field(m.text, sizeof m.text);
        break;
    }
    case MsgType::OrderModified: {
        if (blen < sizeof(boe::OrderModified)) return;
        boe::OrderModified m; std::memcpy(&m, body, sizeof m);
        r.cl_ord_id = cl(m.cl_ord_id); r.order_id = m.order_id; r.qty = m.qty;
        r.price = m.price; r.leaves = m.leaves_qty;
        break;
    }
    case MsgType::OrderCancelled: {
        if (blen < sizeof(boe::OrderCancelled)) return;
        boe::OrderCancelled m; std::memcpy(&m, body, sizeof m);
        r.cl_ord_id = cl(m.cl_ord_id); r.order_id = m.order_id; r.reason = m.reason;
        break;
    }
    case MsgType::OrderExecution: {
        if (blen < sizeof(boe::OrderExecution)) return;
        boe::OrderExecution m; std::memcpy(&m, body, sizeof m);
        r.cl_ord_id = cl(m.cl_ord_id); r.order_id = m.order_id; r.match_id = m.match_id;
        r.qty = m.last_qty; r.price = m.last_price; r.leaves = m.leaves_qty;
        break;
    }
    default:
        return;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        inbox_.push_back(std::move(r));
    }
    cv_.notify_all();
}

/* ---------------- report routing ---------------- */

std::string RemoteSession::next_cl_ord_id() {
    std::string s = "cli" + std::to_string(++cl_counter_);
    s.resize(boe::kClOrdIdLen, ' ');
    return s;
}

// Reports not addressed to the command in flight: executions become fills,
// cancels become notices.
void RemoteSession::route_other(const Report &r) {
    using boe::MsgType;
    if (r.type == static_cast<std::uint8_t>(MsgType::OrderExecution)) {
        fills_.push_back({r.order_id, r.price, r.qty, r.match_id});
        if (r.leaves == 0) {
            auto it = cl_of_order_.find(r.order_id);
            if (it != cl_of_order_.end()) {
                order_of_cl_.erase(it->second);
                cl_of_order_.erase(it);
            }
        }
    } else if (r.type == static_cast<std::uint8_t>(MsgType::OrderCancelled)) {
        notices_.push_back("order " + std::to_string(r.order_id) +
                           " cancelled by exchange (reason '" +
                           std::string(1, static_cast<char>(r.reason)) + "')");
        auto it = cl_of_order_.find(r.order_id);
        if (it != cl_of_order_.end()) {
            order_of_cl_.erase(it->second);
            cl_of_order_.erase(it);
        }
    }
}

bool RemoteSession::await(const std::string &cl, Report &out, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        while (!inbox_.empty()) {
            Report r = std::move(inbox_.front());
            inbox_.pop_front();
            bool mine = cl.empty() ? r.cl_ord_id.empty() : r.cl_ord_id == cl;
            if (mine) {
                out = std::move(r);
                return true;
            }
            route_other(r);
        }
        if (!connected_)
            return false;
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout && inbox_.empty())
            return false;
    }
}

// After an ack/modified: gather the executions and any cancel that follow
// immediately. Stops after a short quiet period or when nothing is left.
void RemoteSession::settle(const std::string &cl, std::uint64_t order_id,
                           std::uint64_t &executed, std::uint32_t &leaves,
                           bool &cancelled, std::uint8_t &cancel_reason) {
    using boe::MsgType;
    Report r;
    while (await(cl, r, 30)) {
        if (r.type == static_cast<std::uint8_t>(MsgType::OrderExecution)) {
            executed += r.qty;
            leaves = r.leaves;
            fills_.push_back({order_id, r.price, r.qty, r.match_id});
        } else if (r.type == static_cast<std::uint8_t>(MsgType::OrderCancelled)) {
            cancelled = true;
            cancel_reason = r.reason;
            leaves = 0;
        } else {
            route_other(r);
        }
        if (leaves == 0 && (cancelled || executed > 0))
            break;
    }
    if (leaves == 0 || cancelled) {
        order_of_cl_.erase(cl);
        cl_of_order_.erase(order_id);
    }
}

/* ---------------- Session API ---------------- */

OrderOutcome RemoteSession::Submit(const std::string &ticker, Side side,
                                   Quantity qty, Price price, OrderType type,
                                   TypeInForce tif) {
    using boe::MsgType;
    OrderOutcome o;
    if (!connected_) {
        o.status = StatusCode::Failed;
        o.detail = "not connected";
        return o;
    }
    std::string cl = next_cl_ord_id();
    boe::NewOrder m{};
    std::memcpy(m.cl_ord_id, cl.data(), boe::kClOrdIdLen);
    m.side = side == Side::Buy ? boe::side::Buy : boe::side::Sell;
    m.qty = static_cast<std::uint32_t>(qty);
    m.price = price;
    put_field(m.symbol, sizeof m.symbol, ticker);
    m.ord_type = type == OrderType::MARKET ? boe::ord_type::Market : boe::ord_type::Limit;
    m.tif = tif == TypeInForce::IOC ? boe::tif::IOC
            : tif == TypeInForce::FOK ? boe::tif::FOK
                                      : boe::tif::GTC;
    send_frame(static_cast<std::uint8_t>(MsgType::NewOrder), &m, sizeof m, true);

    Report r;
    if (!await(cl, r, 3000)) {
        o.status = StatusCode::Failed;
        o.detail = connected_ ? "timed out waiting for a response" : logout_text_;
        return o;
    }
    if (r.type == static_cast<std::uint8_t>(MsgType::OrderRejected)) {
        o.status = status_from_reject(r.reason);
        o.detail = r.text;
        return o;
    }
    // Acknowledged
    o.order_id = r.order_id;
    cl_of_order_[r.order_id] = cl;
    order_of_cl_[cl] = r.order_id;

    std::uint64_t executed = 0;
    std::uint32_t leaves = r.leaves;
    bool cancelled = false;
    std::uint8_t reason = 0;
    if (leaves < qty || tif != TypeInForce::GTC || type == OrderType::MARKET)
        settle(cl, r.order_id, executed, leaves, cancelled, reason);

    o.status = StatusCode::Success;
    o.executed = executed;
    o.remaining = cancelled ? qty - executed : leaves;
    o.resting = !cancelled && leaves > 0;
    if (executed == 0 && o.resting) o.fill = FillStatus::Accepted;
    else if (executed >= qty) o.fill = FillStatus::FullyFilled;
    else if (executed > 0) o.fill = FillStatus::PartiallyFilled;
    else o.fill = FillStatus::Rejected; // nothing done (e.g. market, empty book)
    if (cancelled && reason == boe::cancel_reason::FokFailed) {
        o.status = StatusCode::FOKFailed;
        o.fill = FillStatus::Rejected;
    }
    return o;
}

StatusCode RemoteSession::Cancel(OrderId id) {
    using boe::MsgType;
    if (!connected_)
        return StatusCode::Failed;
    auto it = cl_of_order_.find(id);
    if (it == cl_of_order_.end())
        return StatusCode::OrderNotFound; // not ours, or already gone
    boe::CancelOrder m{};
    std::memcpy(m.orig_cl_ord_id, it->second.data(), boe::kClOrdIdLen);
    send_frame(static_cast<std::uint8_t>(MsgType::CancelOrder), &m, sizeof m, true);

    Report r;
    if (!await(it->second, r, 3000))
        return StatusCode::Failed;
    if (r.type == static_cast<std::uint8_t>(MsgType::OrderRejected))
        return status_from_reject(r.reason);
    if (r.type == static_cast<std::uint8_t>(MsgType::OrderCancelled)) {
        order_of_cl_.erase(it->second);
        cl_of_order_.erase(it);
        return StatusCode::Success;
    }
    route_other(r);
    return StatusCode::Failed;
}

StatusCode RemoteSession::Modify(OrderId id, Quantity qty,
                                 std::optional<Price> price) {
    using boe::MsgType;
    if (!connected_)
        return StatusCode::Failed;
    auto it = cl_of_order_.find(id);
    if (it == cl_of_order_.end())
        return StatusCode::OrderNotFound;
    std::string orig = it->second;
    std::string cl = next_cl_ord_id();
    boe::ModifyOrder m{};
    std::memcpy(m.cl_ord_id, cl.data(), boe::kClOrdIdLen);
    std::memcpy(m.orig_cl_ord_id, orig.data(), boe::kClOrdIdLen);
    m.qty = static_cast<std::uint32_t>(qty);
    m.price = price.value_or(0);
    send_frame(static_cast<std::uint8_t>(MsgType::ModifyOrder), &m, sizeof m, true);

    Report r;
    if (!await(cl, r, 3000))
        return StatusCode::Failed;
    if (r.type == static_cast<std::uint8_t>(MsgType::OrderRejected))
        return status_from_reject(r.reason);
    if (r.type != static_cast<std::uint8_t>(MsgType::OrderModified)) {
        route_other(r);
        return StatusCode::Failed;
    }
    // The order now reports under the new cl_ord_id
    order_of_cl_.erase(orig);
    cl_of_order_[id] = cl;
    order_of_cl_[cl] = id;

    std::uint64_t executed = 0;
    std::uint32_t leaves = r.leaves;
    bool cancelled = false;
    std::uint8_t reason = 0;
    if (leaves < qty)
        settle(cl, id, executed, leaves, cancelled, reason);
    return StatusCode::Success;
}

std::vector<Fill> RemoteSession::TakeFills() {
    // Pull anything the reader has queued that isn't for a command in flight
    {
        std::unique_lock<std::mutex> lk(mu_);
        while (!inbox_.empty()) {
            Report r = std::move(inbox_.front());
            inbox_.pop_front();
            lk.unlock();
            route_other(r);
            lk.lock();
        }
    }
    std::vector<Fill> out;
    out.swap(fills_);
    return out;
}

std::vector<std::string> RemoteSession::TakeNotices() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.swap(notices_);
    return out;
}
