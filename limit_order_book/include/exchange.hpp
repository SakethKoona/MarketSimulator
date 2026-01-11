
#include "engine.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>

class Exchange {
  public:
    Exchange();

    // Exchange(const Exchange &) = default;
    // Exchange(Exchange &&) = default;
    // Exchange &operator=(const Exchange &) = default;
    // Exchange &operator=(Exchange &&) = default;
  private:
    // stores conversion between a named symbol to the symbol id
    std::unordered_map<std::string, SymbolId> stock_registry_;
    MatchingEngine engine_;
};
