#include "retransmit_server.hpp"
#include "protocol/feed.hpp"
#include "protocol/moldudp64.hpp"
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

RetransmitServer::RetransmitServer(const RetransmitStore &store, std::string session,
                                   std::uint16_t port, std::string bind)
    : store_(store), session_(std::move(session)), port_(port), bind_(std::move(bind)) {
    session_.resize(mold::kSessionLen, ' ');
}

RetransmitServer::~RetransmitServer() { stop(); }

void RetransmitServer::start() {
    if (running_.exchange(true))
        return;
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("retransmit socket: ") + std::strerror(errno));
    int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port_);
    if (inet_pton(AF_INET, bind_.c_str(), &a.sin_addr) != 1)
        throw std::runtime_error("retransmit: bad bind address " + bind_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 || ::listen(listen_fd_, 32) < 0)
        throw std::runtime_error(std::string("retransmit bind/listen: ") + std::strerror(errno));
    ::fcntl(listen_fd_, F_SETFL, ::fcntl(listen_fd_, F_GETFL, 0) | O_NONBLOCK);
    thread_ = std::thread([this] { run(); });
}

void RetransmitServer::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
    listen_fd_ = -1;
}

void RetransmitServer::run() {
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
        // Requests are tiny and clients are few: serve inline, bounded by a
        // receive timeout so a stalled client cannot hold the thread.
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

void RetransmitServer::serve(int fd) {
    mold::RetransmitRequest req{};
    std::size_t got = 0;
    while (got < sizeof(req)) {
        ssize_t r = ::recv(fd, reinterpret_cast<char *>(&req) + got, sizeof(req) - got, 0);
        if (r <= 0)
            return;
        got += static_cast<std::size_t>(r);
    }
    requests_.fetch_add(1, std::memory_order_relaxed);
    char sess[mold::kSessionLen];
    std::memcpy(sess, session_.data(), mold::kSessionLen);
    mold::PacketBuilder pb(sess);

    std::uint64_t seq = req.sequence_number;
    std::uint64_t count = std::min<std::uint64_t>(req.message_count, 1000);
    const std::uint64_t oldest = store_.oldest();
    const std::uint64_t newest = store_.newest();
    if (seq == 0 || seq < oldest || seq > newest || count == 0) {
        // Out of range: a count-0 packet whose sequence says where we are.
        pb.control(seq < oldest ? oldest : newest + 1, 0);
        send_all(fd, pb.data(), pb.size());
        return;
    }
    std::uint64_t end = std::min(newest, seq + count - 1);
    char msg[feed::kMaxMessageSize];
    std::uint16_t len;
    pb.begin(seq);
    for (std::uint64_t s = seq; s <= end; ++s) {
        if (!store_.get(s, msg, len))
            break; // overwritten while serving; stop at what we have
        if (!pb.append(msg, len)) {
            if (!send_all(fd, pb.data(), pb.size()))
                return;
            pb.begin(s);
            pb.append(msg, len);
        }
        messages_.fetch_add(1, std::memory_order_relaxed);
    }
    if (!pb.empty())
        send_all(fd, pb.data(), pb.size());
}
