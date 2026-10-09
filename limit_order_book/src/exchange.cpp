#include "exchange.hpp"
#include "common.hpp"
#include "engine.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

json Exchange::LoadConfig(const std::string &config_path) {
    std::vector<std::string> candidates;
    if (!config_path.empty()) {
        candidates.push_back(config_path);
    } else {
        // Conventional locations relative to where the binary is launched
        candidates = {"configs/default.json", "../configs/default.json",
                      "../../configs/default.json"};
    }

    for (const auto &path : candidates) {
        if (std::filesystem::exists(path)) {
            std::ifstream infile(path);
            return json::parse(infile);
        }
    }

    std::string msg = "Exchange config not found; tried:";
    for (const auto &path : candidates)
        msg += " " + path;
    throw std::runtime_error(msg);
}

Exchange::Exchange(const std::string &config_path)
    : Exchange(LoadConfig(config_path)) {}

Exchange::Exchange(const json &cfg)
    : sink_(cfg["sink_size"].get<std::size_t>()),
      sequencer_(cfg["num_symbols"].get<std::size_t>()), // one counter per symbol
      engine_(sink_, sequencer_) {

    // Populate the stock_registry
    SymbolId nextSymId = 0;
    json valid_sym = cfg["symbols"];
    for (auto &[stock, _] : valid_sym.items()) {
        stock_registry_.emplace(stock, nextSymId++);
    }

    engine_.InitBooks(nextSymId);
}

std::optional<SymbolId> Exchange::ResolveSymbol(const Symbol &symbol) const {
    auto it = stock_registry_.find(symbol);
    if (it == stock_registry_.end())
        return std::nullopt;
    return it->second;
}

const std::unordered_map<Symbol, SymbolId> &Exchange::Symbols() const {
    return stock_registry_;
}

/* ---------------- Symbol-name API ---------------- */

FillResult Exchange::SubmitOrder(const Symbol &symbol, Price price,
                                 Quantity qty, Side side, OrderType type,
                                 TypeInForce tif) {
    auto sym_id = ResolveSymbol(symbol);
    if (!sym_id) {
        FillResult res(0);
        res.status_code = StatusCode::SymbolNotFound;
        res.fill_status = FillStatus::Rejected;
        res.qty_remaining = qty;
        return res;
    }
    return SubmitOrder(*sym_id, price, qty, side, type, tif);
}

StatusCode Exchange::L2Snapshot(const Symbol &symbol) {
    auto sym_id = ResolveSymbol(symbol);
    if (!sym_id)
        return StatusCode::SymbolNotFound;

    std::cout << "\n"
              << COLORS::bold << COLORS::cyan << "         ╔════════════════ "
              << symbol << " L2 ════════════════╗" << COLORS::reset << "\n\n";
    return L2Snapshot(*sym_id);
}

StatusCode Exchange::DisplayBook(const Symbol &symbol) {
    auto sym_id = ResolveSymbol(symbol);
    if (!sym_id)
        return StatusCode::SymbolNotFound;
    return DisplayBook(*sym_id);
}

/* ---------------- SymbolId API ---------------- */

FillResult Exchange::SubmitOrder(SymbolId sym_id, Price price, Quantity qty,
                                 Side side, OrderType type, TypeInForce tif) {
    return engine_.SubmitOrder(sym_id, price, qty, side, type, tif);
}

StatusCode Exchange::L2Snapshot(SymbolId sym_id) {
    return engine_.L2Snapshot(sym_id);
}

StatusCode Exchange::DisplayBook(SymbolId sym_id) {
    return engine_.DisplayBook(sym_id);
}

StatusCode Exchange::CancelOrder(OrderId id) { return engine_.CancelOrder(id); }

StatusCode Exchange::ModifyOrder(OrderId id, Quantity newQty,
                                 std::optional<Price> newPrice) {
    return engine_.ModifyOrder(id, newQty, newPrice);
}

/* ---------------- Events ---------------- */

OutBoundEvent *Exchange::NextEvent() { return sink_.consume(); }

void Exchange::DrainEvents(std::ostream &os) {
    while (OutBoundEvent *e = sink_.consume()) {
        os << *e << "\n";
    }
}
