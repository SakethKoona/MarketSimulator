#include "snapshot_service.hpp"
#include "common.hpp"
#include "feed_encoder.hpp"
#include "protocol/feed.hpp"
#include "protocol/moldudp64.hpp"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

SnapshotService::SnapshotService(Exchange &ex, std::string session, std::uint16_t port, std::string bind)
    : ex_(ex), session_(std::move(session)), port_(port), bind_(std::move(bind)),
      shard_symbols_(ex.NumShards()), responses_(1024) {
    session_.resize(mold::kSessionLen, ' ');
    for (std::size_t i = 0; i < ex_.NumShards(); ++i)
        requests_by_shard_.push_back(std::make_unique<MpmcRing<Request>>(64));
    for (const auto &[name, id] : ex_.Symbols())
        shard_symbols_[ex_.ShardOf(id)].push_back(static_cast<std::uint32_t>(id));
    for (auto &v : shard_symbols_)
        std::sort(v.begin(), v.end());
}

SnapshotService::~SnapshotService() { stop(); }

void SnapshotService::start() {
    if (running_.exchange(true))
        return;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("snapshot socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    if (inet_pton(AF_INET, bind_.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("snapshot: bad bind address " + bind_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 || ::listen(listen_fd_, 32) < 0)
        throw std::runtime_error(std::string("snapshot bind/listen: ") + std::strerror(errno));
    ::fcntl(listen_fd_, F_SETFL, ::fcntl(listen_fd_, F_GETFL, 0) | O_NONBLOCK);
    thread_ = std::thread([this] { run(); });
}

void SnapshotService::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
    listen_fd_ = -1;
    Chunk *c;
    while (responses_.try_pop(c))
        delete c;
}

// ---- shard side -----------------------------------------------------------

void SnapshotService::encode_symbol(std::uint32_t sym, std::uint64_t as_of, std::vector<char> &out) {
    const OrderBook &book = ex_.GetBook(sym);
    const std::uint64_t ts = get_current_timestamp();
    char sess[mold::kSessionLen];
    std::memcpy(sess, session_.data(), mold::kSessionLen);
    mold::PacketBuilder pb(sess);
    auto flush = [&] {
        if (!pb.empty()) {
            out.insert(out.end(), pb.data(), pb.data() + pb.size());
            pb.begin(0);
        }
    };
    auto put = [&](const void *m, std::uint16_t len) {
        if (!pb.append(m, len)) {
            flush();
            pb.append(m, len);
        }
    };
    // Count first so the start message is right.
    std::uint32_t count = 0;
    for (auto *n = book.bids().GetHead(); n; n = n->forward[0])
        count += static_cast<std::uint32_t>(n->value.GetSize());
    for (auto *n = book.asks().GetHead(); n; n = n->forward[0])
        count += static_cast<std::uint32_t>(n->value.GetSize());

    pb.begin(0);
    feed::SnapshotStart st{};
    st.symbol_id = sym;
    st.book_seq = as_of;
    st.order_count = count;
    st.ts_ns = ts;
    put(&st, sizeof(st));
    char buf[feed::kMaxMessageSize];
    auto emit_side = [&](const Book &side) {
        for (auto *n = side.GetHead(); n; n = n->forward[0]) {
            for (const OrderNode &o : n->value.orders()) {
                OrderBookEvent e{};
                e.symbol_id = sym;
                e.order_id = o.orderId;
                e.action = BookAction::Add;
                e.side = o.side;
                e.price = o.price;
                e.qty = o.quantity;
                e.book_seq = as_of;
                e.ts_ns = ts;
                put(buf, feed::encode_add(e, buf));
            }
        }
    };
    emit_side(book.bids());
    emit_side(book.asks());
    feed::SnapshotEnd en{};
    en.symbol_id = sym;
    en.order_count = count;
    en.ts_ns = ts;
    put(&en, sizeof(en));
    flush();
}

void SnapshotService::serve_shard(std::size_t shard) {
    Request r;
    auto &ring = *requests_by_shard_[shard];
    while (ring.try_pop(r)) {
        auto *c = new Chunk{r.id, {}};
        const std::uint64_t as_of = ex_.Shard(shard).LastBookSeq();
        for (std::uint32_t sym : shard_symbols_[shard])
            if (r.symbol == feed::kAllSymbols || r.symbol == sym)
                encode_symbol(sym, as_of, c->bytes);
        if (!responses_.try_push(c))
            delete c;
    }
}

// ---- server side ----------------------------------------------------------

void SnapshotService::run() {
    while (running_.load(std::memory_order_relaxed)) {
        pollfd p{listen_fd_, POLLIN, 0};
        if (::poll(&p, 1, 100) <= 0)
            continue;
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0)
            continue;
        // BSD/macOS: accepted sockets inherit O_NONBLOCK from the listener;
        // this connection is served with blocking I/O.
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
        timeval tv{2, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        serve(fd);
        ::close(fd);
    }
}

static bool send_all(int fd, const char *p, std::size_t n) {
    while (n) {
        ssize_t w = ::send(fd, p, n, 0);
        if (w <= 0)
            return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

void SnapshotService::serve(int fd) {
    feed::SnapshotRequest req{};
    std::size_t got = 0;
    while (got < sizeof(req)) {
        ssize_t r = ::recv(fd, reinterpret_cast<char *>(&req) + got, sizeof(req) - got, 0);
        if (r <= 0)
            return;
        got += static_cast<std::size_t>(r);
    }
    requests_.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t id = next_id_++;
    std::size_t expected = 0;
    for (std::size_t sh = 0; sh < requests_by_shard_.size(); ++sh) {
        bool wants = req.symbol_id == feed::kAllSymbols;
        if (!wants)
            for (std::uint32_t s : shard_symbols_[sh])
                wants |= s == req.symbol_id;
        if (wants && requests_by_shard_[sh]->try_push(Request{id, req.symbol_id}))
            ++expected;
    }
    // Collect one chunk per shard, in shard order, with a deadline.
    std::vector<std::unique_ptr<Chunk>> chunks;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (chunks.size() < expected && std::chrono::steady_clock::now() < deadline) {
        Chunk *c;
        if (responses_.try_pop(c)) {
            if (c->id == id)
                chunks.emplace_back(c);
            else
                delete c; // left over from a timed-out request
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    for (auto &c : chunks)
        if (!send_all(fd, c->bytes.data(), c->bytes.size()))
            return;
}
