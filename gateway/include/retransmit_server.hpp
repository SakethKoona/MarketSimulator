#pragma once
// MoldUDP64 retransmission server (feed-v1.md §2.1): a TCP service that
// answers {session, seq, count} with the requested messages re-framed as
// Mold packets, from the publisher's RetransmitStore.
#include "feed_publisher.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

class RetransmitServer {
  public:
    RetransmitServer(const RetransmitStore &store, std::string session, std::uint16_t port,
                     std::string bind = "0.0.0.0");
    ~RetransmitServer();
    void start();
    void stop();
    std::uint64_t requests() const { return requests_.load(); }
    std::uint64_t messages_sent() const { return messages_.load(); }

  private:
    void run();
    void serve(int fd);

    const RetransmitStore &store_;
    std::string session_;
    std::uint16_t port_;
    std::string bind_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requests_{0}, messages_{0};
};
