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
          std::cout << "got the data" << std::endl;
          return data;
      }()) 
{}

Exchange::Exchange(const json &cfg)
    : sink_(cfg["sink_size"].get<std::size_t>()), engine_(sink_) {
    // Populate the stock_registry
    SymbolId nextSymId = 0;
    json valid_sym = cfg["symbols"];
    for (auto &[stock, _] : valid_sym.items()) {
        stock_registry_.emplace(stock, nextSymId);
        // std::cout << stock << nextSymId << std::endl;
        nextSymId++;
    }

    for (const auto& [k, v] : stock_registry_) {
    std::cout << "addr=" << static_cast<const void*>(k.data())
              << " val=" << k << std::endl;
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
    for (const auto& [k, _] : stock_registry_) {
        std::cout << "k == \"AAPL\" ? " << (k == "AAPL") << std::endl;
    }

    std::cout << "hash(map key) = " << std::hash<Symbol>{}("AAPL") << std::endl;
    std::cout << stock_registry_.size() << std::endl; // if added in SubmitOrder


    for (const auto& [k, _] : stock_registry_) {
        std::cout << "hash(stored key) = " << std::hash<Symbol>{}(k) << std::endl;
    }

    auto it = stock_registry_.find(symbol);
    if (it == stock_registry_.end()) {
        throw std::runtime_error("Symbol not found");
    }

    SymbolId sym_id = it->second;

    return engine_.SubmitOrder(sym_id, price, qty, side, type, tif);
}

EngineResult Exchange::CancelOrder(OrderId id) {
    return engine_.CancelOrder(id);
}

EngineResult Exchange::ModifyOrder(OrderId id, Quantity newQty, std::optional<Price> newPrice) {
    return engine_.ModifyOrder(id, newQty, newPrice);
}

void Exchange::L2Snapshot(Symbol symbol) {
    auto it = stock_registry_.find(symbol);
    if (it == stock_registry_.end()) {
        throw std::runtime_error("Symbol Not Found");
    }

    SymbolId symid = it->second;
    engine_.L2Snapshot(symid);
}