#include "exchange.hpp"
#include "events.hpp"
#include <fstream>

Exchange::Exchange() : 
    Exchange([] {
        std::ifstream infile("../configs/default.json");
        json data = json::parse(infile);
        return data;
    }()) {}

Exchange::Exchange(const json& cfg) :
    sink_(cfg["sink_size"].get<std::size_t>()),
    engine_(sink_)
{
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
