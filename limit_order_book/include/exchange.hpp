#pragma once

#include "engine.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

class Exchange {
  public:
    Exchange();
    Exchange(const json &cfg);

    SubmitResult SubmitOrder(Symbol symbol, Price price, Quantity qty,
                             Side side, OrderType type = OrderType::LIMIT,
                             TypeInForce tif = TypeInForce::GTC);

    StatusCode CancelOrder(OrderId id);
    StatusCode ModifyOrder(OrderId id, Quantity newQty,
                           std::optional<Price> newPrice = std::nullopt);
    void L2Snapshot(Symbol symbol);

  private:
    // stores conversion between a named symbol to the symbol id
    std::unordered_map<Symbol, SymbolId> stock_registry_;
    EventSink sink_;
    MatchingEngine engine_;
};
