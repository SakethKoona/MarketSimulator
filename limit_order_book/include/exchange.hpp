
#pragma once

#include "engine.hpp"
#include "orderbook.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

class Exchange {
  public:
    Exchange();
    Exchange(const json &cfg);

    // Exchange(const Exchange &) = default;
    // Exchange(Exchange &&) = default;
    // Exchange &operator=(const Exchange &) = default;
    // Exchange &operator=(Exchange &&) = default;
    SubmitResult SubmitOrder(Symbol symbol, Price price, Quantity qty,
                             Side side, OrderType type = OrderType::LIMIT,
                             TypeInForce tif = TypeInForce::GTC);

  private:
    // stores conversion between a named symbol to the symbol id
    std::unordered_map<std::string, SymbolId> stock_registry_;
    EventSink sink_;
    MatchingEngine engine_;
};
