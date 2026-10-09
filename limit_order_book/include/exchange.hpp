#pragma once

#include "engine.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include "results.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

class Exchange {
  public:
    // Loads a JSON config. The default looks in a few conventional places
    // relative to the working directory and throws if none exist.
    explicit Exchange(const std::string &config_path = "");
    explicit Exchange(const json &cfg);

    // Symbol-name API (resolves the name, then forwards). Unknown symbols
    // produce a SymbolNotFound result; nothing here throws.
    FillResult SubmitOrder(const Symbol &symbol, Price price, Quantity qty,
                           Side side, OrderType type = OrderType::LIMIT,
                           TypeInForce tif = TypeInForce::GTC);
    StatusCode L2Snapshot(const Symbol &symbol);
    StatusCode DisplayBook(const Symbol &symbol);

    // SymbolId API for callers (gateways) that resolve names themselves.
    FillResult SubmitOrder(SymbolId sym_id, Price price, Quantity qty,
                           Side side, OrderType type = OrderType::LIMIT,
                           TypeInForce tif = TypeInForce::GTC);
    StatusCode L2Snapshot(SymbolId sym_id);
    StatusCode DisplayBook(SymbolId sym_id);

    StatusCode CancelOrder(OrderId id);
    StatusCode ModifyOrder(OrderId id, Quantity newQty,
                           std::optional<Price> newPrice = std::nullopt);

    // Symbol registry, for session bootstrap messages
    std::optional<SymbolId> ResolveSymbol(const Symbol &symbol) const;
    const std::unordered_map<Symbol, SymbolId> &Symbols() const;

    // ---- Sharding ----
    // Symbols are spread over NumShards() engines; symbol s lives on shard
    // s % NumShards(). Each shard has its own engine and EventSink, and
    // only one thread may call into a given shard. The routing methods
    // above (by SymbolId or OrderId) dispatch to the right shard; the
    // Exchange itself takes no locks.
    std::size_t NumShards() const { return shards_.size(); }
    ShardId ShardOf(SymbolId sym_id) const {
        return static_cast<ShardId>(sym_id % shards_.size());
    }
    MatchingEngine &Shard(std::size_t i) { return shards_[i]->engine; }
    const MatchingEngine &Shard(std::size_t i) const { return shards_[i]->engine; }

    // The event sink of a shard, for a publisher thread that consumes it
    // directly. Sink() is shard 0, which is the only sink at shards=1.
    EventSink &Sink(std::size_t shard) { return shards_[shard]->sink; }
    EventSink &Sink() { return Sink(0); }

    // Read-only view of a book, for conformance tests and snapshots.
    const OrderBook &GetBook(SymbolId sym_id) const {
        return shards_[ShardOf(sym_id)]->engine.GetBook(sym_id);
    }

    // Pops the next published event from any shard (round-robin), or
    // nullptr when every sink is empty. Single-threaded convenience for
    // tests and tools; a real publisher consumes Sink(i) per shard.
    OutBoundEvent *NextEvent();
    // Pops and prints every pending event
    void DrainEvents(std::ostream &os);

  private:
    static json LoadConfig(const std::string &config_path);

    struct ShardState {
        EventSink sink;
        MatchingEngine engine;
        ShardState(std::size_t sink_size, ShardId id)
            : sink(sink_size), engine(sink, id) {}
    };

    MatchingEngine &engine_for_symbol(SymbolId sym_id) {
        return shards_[ShardOf(sym_id)]->engine;
    }
    MatchingEngine *engine_for_order(OrderId id) {
        ShardId s = shard_of(id);
        return s < shards_.size() ? &shards_[s]->engine : nullptr;
    }

    // stores conversion between a named symbol to the symbol id
    std::unordered_map<Symbol, SymbolId> stock_registry_;
    std::vector<std::unique_ptr<ShardState>> shards_;
    std::size_t next_drain_shard_ = 0;
};
