#pragma once

#include "engine.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include "results.hpp"
#include "sequencer.hpp"
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

    // The event sink, for a publisher thread that consumes it directly.
    EventSink &Sink() { return sink_; }
    // Read-only view of a book, for conformance tests and snapshots.
    const OrderBook &GetBook(SymbolId sym_id) const { return engine_.GetBook(sym_id); }

    // Pops the next published event, or nullptr when the sink is empty
    OutBoundEvent *NextEvent();
    // Pops and prints every pending event
    void DrainEvents(std::ostream &os);

  private:
    static json LoadConfig(const std::string &config_path);

    // stores conversion between a named symbol to the symbol id
    std::unordered_map<Symbol, SymbolId> stock_registry_;
    EventSink sink_;
    Sequencer sequencer_;
    MatchingEngine engine_;
};
