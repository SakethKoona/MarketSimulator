#pragma once
// BOE order-entry gateway: one thread, poll()-driven, owning every TCP
// session. Decodes client messages into InboundCommands for the engine
// thread and turns OrderReports from it into execution reports.
// Contract: docs/protocol/boe-v1.md
#include "commands.hpp"
#include "protocol/boe.hpp"
#include "spsc_ring.hpp"
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct BoeConfig {
    std::uint16_t port = 30000;
    std::string bind = "0.0.0.0";
    std::uint32_t heartbeat_ms = 1000;
    std::uint32_t timeout_ms = 5000;
    std::uint32_t poll_ms = 1;
    std::size_t max_sessions = 64;
};

struct BoeStats {
    std::atomic<std::uint64_t> sessions{0};
    std::atomic<std::uint64_t> logins{0};
    std::atomic<std::uint64_t> orders_in{0};
    std::atomic<std::uint64_t> cancels_in{0};
    std::atomic<std::uint64_t> modifies_in{0};
    std::atomic<std::uint64_t> rejects{0};
    std::atomic<std::uint64_t> reports_out{0};
    std::atomic<std::uint64_t> ring_full{0};
};

class BoeServer {
  public:
    using SymbolMap = std::unordered_map<std::string, SymbolId>;

    // One command ring and one report ring per shard, indexed alike.
    // symbol_shard maps symbol_id -> shard; order ids carry their shard in
    // the top 8 bits (shard_of in common.hpp).
    BoeServer(BoeConfig cfg, std::vector<SpscRing<InboundCommand> *> commands,
              std::vector<SpscRing<OrderReport> *> reports, SymbolMap symbols,
              std::vector<std::uint8_t> symbol_shard);
    ~BoeServer();

    void start();
    void stop();
    const BoeStats &stats() const { return stats_; }

  private:
    struct ClOrdId {
        char v[boe::kClOrdIdLen];
    };
    struct Session {
        int fd = -1;
        std::uint32_t gen = 0; // bumps on reuse so stale ids don't match
        bool logged_in = false;
        std::uint32_t next_in_seq = 1;
        std::uint32_t next_out_seq = 1;
        std::uint64_t last_rx_ns = 0;
        std::uint64_t last_tx_ns = 0;
        std::vector<char> rx;
        std::vector<char> tx;
        std::unordered_map<std::string, std::uint64_t> clords; // cl_ord_id -> order_id
        std::string sub_id;
    };
    struct Pending { // a command awaiting its first report
        std::uint32_t sess;
        std::uint32_t gen;
        ClOrdId cl;      // id to report under
        ClOrdId orig;    // Modify: the id being replaced
        CommandType type;
        std::uint64_t price; // New/Modify: requested price (Modify 0 = keep)
    };
    struct Live { // an acknowledged order
        std::uint32_t sess;
        std::uint32_t gen;
        ClOrdId cl;
        Side side;
        std::uint32_t symbol_id;
        std::uint64_t price;
        std::uint32_t leaves;
    };

    void run();
    void accept_new();
    void on_readable(std::uint32_t si);
    void on_writable(std::uint32_t si);
    void close_session(std::uint32_t si);
    void handle_frame(std::uint32_t si, const boe::Header &h, const char *body,
                      std::uint16_t body_len);
    void handle_new(std::uint32_t si, const boe::NewOrder &m);
    void handle_cancel(std::uint32_t si, const boe::CancelOrder &m);
    void handle_modify(std::uint32_t si, const boe::ModifyOrder &m);
    void drain_reports();
    bool push_command(const InboundCommand &c, std::size_t shard);
    void on_report(const OrderReport &r);

    template <typename Body>
    void send(std::uint32_t si, boe::MsgType type, const Body &b, bool app_msg = true);
    void send_raw(std::uint32_t si, boe::MsgType type, const void *body,
                  std::uint16_t len, bool app_msg);
    void reject(std::uint32_t si, const ClOrdId &cl, std::uint8_t reason,
                const char *text);
    bool session_alive(std::uint32_t si, std::uint32_t gen) const;
    std::uint64_t now_ns() const;

    BoeConfig cfg_;
    std::vector<SpscRing<InboundCommand> *> commands_;
    std::vector<SpscRing<OrderReport> *> reports_;
    SymbolMap symbols_;
    std::vector<std::uint8_t> symbol_shard_;
    std::vector<std::string> tickers_; // symbol_id -> padded ticker
    BoeStats stats_;

    int listen_fd_ = -1;
    std::vector<Session> sessions_;
    std::vector<std::uint32_t> free_slots_;
    std::unordered_map<std::uint64_t, Pending> pending_; // request_id
    std::unordered_map<std::uint64_t, Live> live_;       // order_id
    std::uint64_t next_request_ = 1;

    std::thread thread_;
    std::atomic<bool> running_{false};
};
