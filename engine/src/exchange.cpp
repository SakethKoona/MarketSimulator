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

Exchange::Exchange(const json &cfg) {
    // Populate the stock_registry
    SymbolId nextSymId = 0;
    json valid_sym = cfg["symbols"];
    for (auto &[stock, _] : valid_sym.items()) {
        stock_registry_.emplace(stock, nextSymId++);
    }

    std::size_t sink_size = cfg["sink_size"].get<std::size_t>();
    std::size_t num_shards = cfg.contains("shards")
                                 ? cfg["shards"].get<std::size_t>()
                                 : 1;
    if (num_shards == 0)
        num_shards = 1;
    if (num_shards > kMaxShards)
        throw std::runtime_error("Exchange: at most 256 shards");
    // A shard with no symbols is an idle engine on an idle thread. Each
    // symbol lives on exactly one shard, so parallelism is capped by the
    // symbol count; clamp rather than spin up empty shards.
    if (nextSymId > 0 && num_shards > nextSymId) {
        std::cerr << "Exchange: " << num_shards << " shards requested for "
                  << nextSymId << " symbols; using " << nextSymId << "\n";
        num_shards = nextSymId;
    }

    // One engine + sink per shard; symbol s lives on shard s % num_shards
    std::vector<std::vector<SymbolId>> per_shard(num_shards);
    for (SymbolId s = 0; s < nextSymId; s++)
        per_shard[s % num_shards].push_back(s);

    shards_.reserve(num_shards);
    for (std::size_t i = 0; i < num_shards; i++) {
        shards_.push_back(
            std::make_unique<ShardState>(sink_size, static_cast<ShardId>(i)));
        shards_.back()->engine.InitBooks(per_shard[i], nextSymId);
    }
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

const Symbol &Exchange::TickerOf(SymbolId sym_id) const {
    static const Symbol empty;
    for (const auto &[name, id] : stock_registry_)
        if (id == sym_id)
            return name;
    return empty;
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
    if (sym_id >= stock_registry_.size()) {
        FillResult res(0);
        res.status_code = StatusCode::SymbolNotFound;
        res.fill_status = FillStatus::Rejected;
        res.qty_remaining = qty;
        return res;
    }
    return engine_for_symbol(sym_id).SubmitOrder(sym_id, price, qty, side,
                                                 type, tif);
}

StatusCode Exchange::L2Snapshot(SymbolId sym_id) {
    if (sym_id >= stock_registry_.size())
        return StatusCode::SymbolNotFound;
    return engine_for_symbol(sym_id).L2Snapshot(sym_id);
}

StatusCode Exchange::DisplayBook(SymbolId sym_id) {
    if (sym_id >= stock_registry_.size())
        return StatusCode::SymbolNotFound;
    return engine_for_symbol(sym_id).DisplayBook(sym_id);
}

StatusCode Exchange::CancelOrder(OrderId id) {
    MatchingEngine *eng = engine_for_order(id);
    return eng ? eng->CancelOrder(id) : StatusCode::OrderNotFound;
}

StatusCode Exchange::ModifyOrder(OrderId id, Quantity newQty,
                                 std::optional<Price> newPrice) {
    MatchingEngine *eng = engine_for_order(id);
    return eng ? eng->ModifyOrder(id, newQty, newPrice)
               : StatusCode::OrderNotFound;
}

/* ---------------- Events ---------------- */

OutBoundEvent *Exchange::NextEvent() {
    // Round-robin so no shard starves when several have events pending
    for (std::size_t i = 0; i < shards_.size(); i++) {
        std::size_t s = (next_drain_shard_ + i) % shards_.size();
        if (OutBoundEvent *e = shards_[s]->sink.consume()) {
            next_drain_shard_ = (s + 1) % shards_.size();
            return e;
        }
    }
    return nullptr;
}

void Exchange::DrainEvents(std::ostream &os) {
    while (OutBoundEvent *e = NextEvent()) {
        os << *e << "\n";
    }
}
