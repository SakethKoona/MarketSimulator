
#include "exchange.hpp"
#include "events.hpp"
#include <fstream>

using json = nlohmann::json;

//  TODO: Fix issues here
Exchange::Exchange() {
    std::ifstream infile("../configs/default.json");
    json data = json::parse(infile);

    SymbolId nextSymId = 0;
    json valid_sym = data["symbols"];
    for (auto &[stock, params] : valid_sym.items()) {
        stock_registry_[stock] = nextSymId++;
    }

    // Init the engine
    auto sink_size = data["sink_size"].get<int>();
    EventSink sink = EventSink(sink_size);
    engine_ = MatchingEngine(sink);
}
