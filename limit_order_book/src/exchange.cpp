#include "exchange.hpp"
#include "common.hpp"
#include "engine.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include <fstream>
#include <stdexcept>

Exchange::Exchange()
    : Exchange([] {
          std::ifstream infile("../configs/default.json");
          json data = json::parse(infile);
          return data;
      }()) {}

Exchange::Exchange(const json &cfg)
    : sink_(cfg["sink_size"].get<std::size_t>()),
      sequencer_(cfg["num_symbols"].get<std::size_t>()), // Init one counter for each symbol
      engine_(sink_, sequencer_) {
        
    // Populate the stock_registry
    SymbolId nextSymId = 0;
    json valid_sym = cfg["symbols"];
    for (auto &[stock, _] : valid_sym.items()) {
        stock_registry_.emplace(stock, nextSymId++);
    }

    // Next, we wanna pass nextSymId into the matching engine,
    // so that it can make and keep track of that many orderbooks
    // One option is we can have a function inside matching engine
    // that would take in the size to reserve
    engine_.InitBooks(nextSymId);
}

FillResult Exchange::SubmitOrder(Symbol symbol, Price price, Quantity qty,
                                   Side side, OrderType type, TypeInForce tif) {

    if (!stock_registry_.contains(symbol)) {
        throw std::runtime_error("Symbol Not Found in Stock registry");
    }

    SymbolId sym_id = stock_registry_.at(symbol);
    FillResult res = engine_.SubmitOrder(sym_id, price, qty, side, type, tif);

    return res;
}

StatusCode Exchange::CancelOrder(OrderId id) { return engine_.CancelOrder(id); }

StatusCode Exchange::ModifyOrder(OrderId id, Quantity newQty,
                                 std::optional<Price> newPrice) {
    return engine_.ModifyOrder(id, newQty, newPrice);
}

void Exchange::L2Snapshot(Symbol symbol) {
    auto it = stock_registry_.find(symbol);
    if (it == stock_registry_.end()) {
        throw std::runtime_error("Symbol Not Found From L2 Snapshot");
    }

    std::cout << "\n"
            << COLORS::bold << COLORS::cyan << "         ╔════════════════ "
            << symbol << " L2 ════════════════╗" << COLORS::reset << "\n\n";

    SymbolId symid = it->second;
    engine_.L2Snapshot(symid);
}

void Exchange::DisplayBook(Symbol symbol) {
    auto it = stock_registry_.find(symbol);
    if (it == stock_registry_.end()) {
        throw std::runtime_error("Symbol Not Found from Display Book");
    }

    SymbolId symId = it->second;
    engine_.DisplayBook(symId);
}