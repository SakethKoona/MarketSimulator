#pragma once
// Publisher thread: drains the EventSink, encodes feed messages, frames them
// in MoldUDP64 packets and multicasts them. Also owns the session bootstrap
// (System Event + Stock Directory), heartbeats, and the retransmission
// store that the TCP retransmit server reads.
#include "events.hpp"
#include "feed_framer.hpp"
#include "protocol/feed.hpp"
#include "protocol/moldudp64.hpp"
#include "udp_multicast.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct FeedConfig {
    std::string group = "239.1.1.1";
    std::uint16_t port = 30001;
    std::string interface;      // local IPv4 to send on; "" = kernel default
    std::string session = "MKTSIM0001"; // exactly 10 chars, padded/truncated
    std::uint32_t flush_us = 100;       // max time a partial packet waits
    std::uint32_t heartbeat_ms = 1000;
    std::uint32_t directory_s = 5;      // Stock Directory repeat interval
    std::uint32_t idle_sleep_us = 20;   // publisher sleep when nothing to do
    int ttl = 1;
    bool loop = true;
    std::size_t retransmit_capacity = 1u << 16; // messages kept for replay
};

// Last N encoded messages, indexed by Mold sequence number. Written by the
// publisher, read by the retransmit server; a mutex keeps it simple.
class RetransmitStore {
  public:
    explicit RetransmitStore(std::size_t capacity);
    void put(std::uint64_t seq, const char *data, std::uint16_t len);
    // Copies message `seq` into out; false if it has been overwritten or
    // not yet published. oldest() is the lowest seq still available.
    bool get(std::uint64_t seq, char *out, std::uint16_t &len) const;
    std::uint64_t oldest() const;
    std::uint64_t newest() const;

  private:
    struct Slot {
        std::uint64_t seq = 0;
        std::uint16_t len = 0;
        char data[feed::kMaxMessageSize];
    };
    mutable std::mutex mu_;
    std::vector<Slot> slots_;
    std::uint64_t newest_ = 0;
};

struct FeedStats {
    std::atomic<std::uint64_t> messages{0};
    std::atomic<std::uint64_t> packets{0};
    std::atomic<std::uint64_t> heartbeats{0};
    std::atomic<std::uint64_t> send_errors{0};
    std::atomic<std::uint64_t> engine_drops{0}; // events lost in the sink
    std::atomic<std::uint64_t> unpaired_executes{0};
    std::atomic<std::uint64_t> next_seq{1};
    std::atomic<int> last_errno{0};
};

class FeedPublisher {
  public:
    using SymbolTable = std::vector<std::pair<SymbolId, std::string>>;

    FeedPublisher(EventSink &sink, SymbolTable symbols, FeedConfig cfg);
    ~FeedPublisher();

    void start();
    void stop(); // publishes End of Session, joins the thread

    const FeedStats &stats() const { return stats_; }
    const RetransmitStore &store() const { return store_; }
    const FeedConfig &config() const { return cfg_; }

  private:
    void run();
    void publish(const char *msg, std::uint16_t len); // appends, may flush
    void flush();
    void send_control(std::uint16_t count);
    void send_directory();
    std::uint64_t now_ns() const;

    EventSink &sink_;
    SymbolTable symbols_;
    FeedConfig cfg_;
    net::UdpMulticastSender sock_;
    mold::PacketBuilder packet_;
    RetransmitStore store_;
    FeedStats stats_;
    FeedFramer framer_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::uint64_t next_seq_ = 1;
    std::uint64_t packet_open_ns_ = 0;
    std::uint64_t last_send_ns_ = 0;
    std::uint64_t last_dir_ns_ = 0;
    std::uint64_t seen_drops_ = 0;
    char scratch_[feed::kMaxMessageSize];
};
