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
    SymbolId nextSymId = 0;
    json valid_sym = cfg["symbols"];
    for (auto &[stock, params] : valid_sym.items()) {
        stock_registry_[stock] = nextSymId++;
    }
}
