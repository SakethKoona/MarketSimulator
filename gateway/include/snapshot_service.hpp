#pragma once
// Snapshot server (feed-v1.md §5): a TCP service that sends a symbol's (or
// every symbol's) resting orders as Add Orders bracketed by Snapshot Start
// and End, as of the shard's current book sequence. Books are read only on
// their own shard thread: the server thread queues a request per shard and
// the shard loop answers by calling serve_shard().
#include "exchange.hpp"
#include "mpmc_ring.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class SnapshotService {
  public:
    SnapshotService(Exchange &ex, std::string session, std::uint16_t port, std::string bind = "0.0.0.0");
    ~SnapshotService();
    void start();
    void stop();
    // Called from the thread that owns `shard`, between commands.
    void serve_shard(std::size_t shard);
    std::uint64_t requests() const { return requests_.load(); }

  private:
    struct Request {
        std::uint64_t id;
        std::uint32_t symbol; // feed::kAllSymbols or one
    };
    struct Chunk {
        std::uint64_t id;
        std::vector<char> bytes;
    };
    void run();
    void serve(int fd);
    void encode_symbol(std::uint32_t sym, std::uint64_t as_of, std::vector<char> &out);

    Exchange &ex_;
    std::string session_;
    std::uint16_t port_;
    std::string bind_;
    std::vector<std::vector<std::uint32_t>> shard_symbols_;
    std::vector<std::unique_ptr<MpmcRing<Request>>> requests_by_shard_;
    MpmcRing<Chunk *> responses_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requests_{0};
    std::uint64_t next_id_ = 1;
};
