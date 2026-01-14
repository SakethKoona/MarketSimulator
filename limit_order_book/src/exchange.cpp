#include "exchange.hpp"
#include "engine.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include "types.hpp"
#include <fstream>
#include <stdexcept>

Exchange::Exchange()
    : Exchange([] {
          std::ifstream infile("../configs/default.json");
          json data = json::parse(infile);
          return data;
      }()) {}

Exchange::Exchange(const json &cfg)
    : sink_(cfg["sink_size"].get<std::size_t>()), engine_(sink_) {
    // Populate the stock_registry
    SymbolId nextSymId = 0;
    json valid_sym = cfg["symbols"];
    for (auto &[stock, params] : valid_sym.items()) {
        stock_registry_.emplace(stock, nextSymId++);
    }

    // Next, we wanna pass nextSymId into the matching engine,
    // so that it can make and keep track of that many orderbooks
    // One option is we can have a function inside matching engine
    // that would take in the size to reserve
    engine_.InitBooks(nextSymId);
}

SubmitResult Exchange::SubmitOrder(Symbol symbol, Price price, Quantity qty,
                                   Side side, OrderType type, TypeInForce tif) {

    // First find the symbol id
    auto it = stock_registry_.find(symbol);
    if (it == stock_registry_.end()) {
        throw std::runtime_error("Symbol not found");
    }

    SymbolId sym_id = it->second;

    return engine_.SubmitOrder(sym_id, price, qty, side, type, tif);
}
