#pragma once
// Thin POSIX wrappers for UDP multicast send/receive. No dependencies.
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace net {

inline std::string errno_str(const char *what) {
    return std::string(what) + ": " + std::strerror(errno);
}

inline in_addr parse_ipv4(const std::string &s) {
    in_addr a{};
    if (s.empty()) {
        a.s_addr = htonl(INADDR_ANY);
        return a;
    }
    if (inet_pton(AF_INET, s.c_str(), &a) != 1)
        throw std::runtime_error("bad IPv4 address: " + s);
    return a;
}

class UdpMulticastSender {
  public:
    // iface: local IPv4 of the interface to send on, or "" for the kernel
    // default. loop: deliver to receivers on this host too.
    UdpMulticastSender(const std::string &group, std::uint16_t port,
                       const std::string &iface = "", int ttl = 1,
                       bool loop = true) {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error(errno_str("socket"));
        unsigned char t = static_cast<unsigned char>(ttl);
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &t, sizeof(t));
        unsigned char l = loop ? 1 : 0;
        ::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &l, sizeof(l));
        if (!iface.empty()) {
            in_addr a = parse_ipv4(iface);
            if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a)) < 0)
                throw std::runtime_error(errno_str("IP_MULTICAST_IF"));
        }
        int sndbuf = 4 * 1024 * 1024;
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

        std::memset(&dst_, 0, sizeof(dst_));
        dst_.sin_family = AF_INET;
        dst_.sin_port = htons(port);
        dst_.sin_addr = parse_ipv4(group);
    }
    ~UdpMulticastSender() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    UdpMulticastSender(const UdpMulticastSender &) = delete;
    UdpMulticastSender &operator=(const UdpMulticastSender &) = delete;

    // Returns bytes sent or -1 (errno set). Never throws on the hot path.
    ssize_t send(const void *data, std::size_t len) noexcept {
        return ::sendto(fd_, data, len, 0,
                        reinterpret_cast<const sockaddr *>(&dst_), sizeof(dst_));
    }
    int fd() const { return fd_; }

  private:
    int fd_ = -1;
    sockaddr_in dst_{};
};

class UdpMulticastReceiver {
  public:
    UdpMulticastReceiver(const std::string &group, std::uint16_t port,
                         const std::string &iface = "") {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
            throw std::runtime_error(errno_str("socket"));
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
        int rcvbuf = 8 * 1024 * 1024;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(port);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0)
            throw std::runtime_error(errno_str("bind"));

        ip_mreq m{};
        m.imr_multiaddr = parse_ipv4(group);
        m.imr_interface = parse_ipv4(iface);
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
            throw std::runtime_error(errno_str("IP_ADD_MEMBERSHIP"));
    }
    ~UdpMulticastReceiver() {
        if (fd_ >= 0)
            ::close(fd_);
    }
    UdpMulticastReceiver(const UdpMulticastReceiver &) = delete;
    UdpMulticastReceiver &operator=(const UdpMulticastReceiver &) = delete;

    // Waits up to timeout_ms (-1 forever). Returns bytes, 0 on timeout, -1
    // on error.
    ssize_t recv(void *buf, std::size_t len, int timeout_ms) noexcept {
        pollfd p{fd_, POLLIN, 0};
        int r = ::poll(&p, 1, timeout_ms);
        if (r <= 0)
            return r;
        return ::recvfrom(fd_, buf, len, 0, nullptr, nullptr);
    }
    int fd() const { return fd_; }

  private:
    int fd_ = -1;
};

} // namespace net
