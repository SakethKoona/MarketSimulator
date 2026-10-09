#pragma once
// Session over a BOE/TCP connection to a running exchange_server.
// Contract: docs/protocol/boe-v1.md. Uses the gateway's packed wire structs
// read-only.
#include "session.hpp"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class RemoteSession : public Session {
  public:
    // Connects and logs in; throws on failure.
    RemoteSession(const std::string &host, std::uint16_t port,
                  const std::string &user = "cli ",
                  const std::string &password = "          ");
    ~RemoteSession() override;

    OrderOutcome Submit(const std::string &ticker, Side side, Quantity qty,
                        Price price, OrderType type, TypeInForce tif) override;
    StatusCode Cancel(OrderId id) override;
    StatusCode Modify(OrderId id, Quantity qty,
                      std::optional<Price> price) override;
    std::vector<Fill> TakeFills() override;
    bool CanInspect() const override { return false; }
    std::vector<std::string> Tickers() const override { return {}; }
    std::string Describe() const override;

    // Unsolicited notices (passive cancels, logout) since the last call
    std::vector<std::string> TakeNotices();
    bool Connected() const { return connected_; }

  private:
    // A decoded server→client application message
    struct Report {
        std::uint8_t type;
        std::string cl_ord_id;
        std::uint64_t order_id = 0;
        std::uint64_t match_id = 0;
        std::uint64_t price = 0;
        std::uint32_t qty = 0;
        std::uint32_t leaves = 0;
        std::uint8_t reason = 0;
        std::string text;
    };

    void reader_loop();
    void handle_frame(const char *buf, std::size_t len);
    void send_frame(std::uint8_t type, const void *body, std::size_t len,
                    bool app_msg);
    void send_heartbeat();
    std::string next_cl_ord_id();
    // Waits up to timeout_ms for a report with this cl_ord_id; returns
    // false on timeout or disconnect. Other reports are routed to fills_
    // or notices_.
    bool await(const std::string &cl, Report &out, int timeout_ms);
    // After an ack: collect executions / cancels for cl until quiet
    void settle(const std::string &cl, std::uint64_t order_id,
                std::uint64_t &executed, std::uint32_t &leaves,
                bool &cancelled, std::uint8_t &cancel_reason);
    void route_other(const Report &r);

    int fd_ = -1;
    std::string host_;
    std::uint16_t port_;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    std::mutex tx_mu_;
    std::uint32_t out_seq_ = 1;
    std::uint64_t last_tx_ns_ = 0;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Report> inbox_;
    std::string logout_text_;

    std::uint64_t cl_counter_ = 0;
    std::unordered_map<std::uint64_t, std::string> cl_of_order_; // order_id -> cl
    std::unordered_map<std::string, std::uint64_t> order_of_cl_;
    std::vector<Fill> fills_;
    std::vector<std::string> notices_;
};
