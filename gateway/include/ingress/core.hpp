#pragma once
// Order-entry core: everything every ingress adapter needs and nothing
// protocol-specific. Implements the C API in ingress/api.h on top of the
// Exchange: sessions, per-shard lock-free command queues that any session
// may submit to, a report queue per session, and order→session routing
// kept on the shard engine threads.
#include "exchange.hpp"
#include "ingress/api.h"
#include "mpmc_ring.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct IngressStats {
    std::atomic<std::uint64_t> sessions_opened{0};
    std::atomic<std::uint64_t> sessions_open{0};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> busy{0};         // submit refused: shard queue full
    std::atomic<std::uint64_t> bad_request{0};
    std::atomic<std::uint64_t> applied{0};      // commands the engines consumed
    std::atomic<std::uint64_t> reports{0};
    std::atomic<std::uint64_t> report_drops{0}; // session report queue full
};

class OrderEntryCore {
  public:
    struct Config {
        std::size_t cmd_ring = 1u << 16;   // per shard
        std::size_t report_ring = 1u << 13; // per session
        std::size_t max_sessions = 256;
    };

    OrderEntryCore(Exchange &ex, Config cfg);
    ~OrderEntryCore();
    OrderEntryCore(const OrderEntryCore &) = delete;
    OrderEntryCore &operator=(const OrderEntryCore &) = delete;

    // What adapters get.
    const mktsim_exchange_api *api() const { return &api_; }
    mktsim_exchange *handle() { return reinterpret_cast<mktsim_exchange *>(this); }

    // Engine side: the thread owning `shard` calls this in its loop. Applies
    // up to max_commands and routes the resulting reports and fills.
    std::size_t pump(std::size_t shard, std::size_t max_commands = 256);
    std::size_t num_shards() const { return cmds_.size(); }
    const IngressStats &stats() const { return stats_; }

    // Adapter side (the C API trampolines land here).
    mktsim_session *open_session(const char *name);
    void close_session(mktsim_session *s);
    int submit(mktsim_session *s, const mktsim_order_req &req);
    std::size_t poll(mktsim_session *s, mktsim_report_fn cb, void *user, std::size_t max);
    int resolve_symbol(const char *ticker, std::size_t len, std::uint32_t *out) const;
    std::size_t symbols(mktsim_symbol_info *out, std::size_t max) const;
    void log(const char *adapter, const char *msg) const;

  private:
    struct Session {
        std::uint32_t slot = 0;
        std::atomic<std::uint32_t> gen{0};
        std::atomic<bool> open{false};
        std::unique_ptr<MpmcRing<mktsim_report>> reports;
        std::string name;
    };
    struct Owner {
        std::uint32_t slot;
        std::uint32_t gen;
    };
    struct Command {
        mktsim_order_req req;
        Owner owner;
    };

    bool alive(Owner o) const;
    void push_report(Owner o, mktsim_report r);
    // Routes fills to their owners. If `aggressor` is set, its executions
    // carry a running leaves count starting from `aggressor_qty`.
    void drain_fills(std::size_t shard, std::uint64_t aggressor = 0, std::uint32_t aggressor_qty = 0);
    std::uint32_t resting_leaves(std::uint32_t symbol_id, std::uint64_t order_id) const;
    bool on_book(std::uint32_t symbol_id, std::uint64_t order_id) const;

    Exchange &ex_;
    Config cfg_;
    mktsim_exchange_api api_{};
    std::vector<std::unique_ptr<MpmcRing<Command>>> cmds_; // per shard
    std::vector<std::unique_ptr<Session>> sessions_;       // max_sessions slots
    MpmcRing<std::uint32_t> free_slots_;
    std::vector<std::unordered_map<std::uint64_t, Owner>> owners_; // per shard, engine thread only
    std::vector<std::uint8_t> symbol_shard_;
    std::unordered_map<std::string, std::uint32_t> by_ticker_;
    std::vector<std::string> tickers_;
    IngressStats stats_;
};
