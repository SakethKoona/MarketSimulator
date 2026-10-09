#include "session.hpp"
#include <algorithm>

LocalSession::LocalSession(const json &cfg) : ex_(cfg) {}

void LocalSession::drain() {
    while (OutBoundEvent *e = ex_.NextEvent()) {
        if (auto *t = std::get_if<TradeFillEvent>(e)) {
            fills_.push_back({t->aggressor_id, t->price, t->qty, t->trade_id});
            fills_.push_back({t->resting_id, t->price, t->qty, t->trade_id});
        }
        events_.push_back(*e);
    }
}

OrderOutcome LocalSession::Submit(const std::string &ticker, Side side,
                                  Quantity qty, Price price, OrderType type,
                                  TypeInForce tif) {
    FillResult r = ex_.SubmitOrder(ticker, price, qty, side, type, tif);
    drain();
    OrderOutcome o;
    o.status = r.status_code;
    o.fill = r.fill_status;
    o.order_id = r.order_id;
    o.executed = r.qty_executed;
    o.remaining = r.qty_remaining;
    o.resting = r.resting;
    return o;
}

StatusCode LocalSession::Cancel(OrderId id) {
    StatusCode sc = ex_.CancelOrder(id);
    drain();
    return sc;
}

StatusCode LocalSession::Modify(OrderId id, Quantity qty,
                                std::optional<Price> price) {
    StatusCode sc = ex_.ModifyOrder(id, qty, price);
    drain();
    return sc;
}

std::vector<Fill> LocalSession::TakeFills() {
    drain();
    std::vector<Fill> out;
    out.swap(fills_);
    return out;
}

std::vector<OutBoundEvent> LocalSession::TakeEvents() {
    drain();
    std::vector<OutBoundEvent> out;
    out.swap(events_);
    return out;
}

std::vector<std::string> LocalSession::Tickers() const {
    std::vector<std::pair<SymbolId, std::string>> v;
    for (const auto &[name, id] : ex_.Symbols())
        v.emplace_back(id, name);
    std::sort(v.begin(), v.end());
    std::vector<std::string> out;
    for (auto &[id, name] : v)
        out.push_back(name);
    return out;
}

std::string LocalSession::Describe() const {
    return "in-process exchange, " + std::to_string(ex_.Symbols().size()) +
           " symbols, " + std::to_string(ex_.NumShards()) + " shard(s)";
}
