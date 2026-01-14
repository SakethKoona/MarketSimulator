
#pragma once

#include "engine.hpp"
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
  private:
    // stores conversion between a named symbol to the symbol id
    std::unordered_map<std::string, SymbolId> stock_registry_;
    MatchingEngine engine_;
    EventSink sink_;
};
