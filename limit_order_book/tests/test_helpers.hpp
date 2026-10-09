#pragma once
// Shared fixtures for exchange-level tests.
#include "exchange.hpp"
#include "test_framework.hpp"
#include <map>

// Builds an Exchange from an inline config so tests don't depend on cwd.
inline Exchange make_exchange(std::size_t sink_size = 4096) {
    json cfg = {{"symbols", {{"AAPL", json::object()}, {"GOOG", json::object()}}},
                {"sink_size", sink_size},
                {"seq_capacity", 2},
                {"num_symbols", 2}};
    return Exchange(cfg);
}

// Pops every pending event into a vector.
inline std::vector<OutBoundEvent> drain(Exchange &ex) {
    std::vector<OutBoundEvent> out;
    while (auto *e = ex.NextEvent())
        out.push_back(*e);
    return out;
}

// A client-side L3 book rebuilt from the event stream only. Used to check
// that the published events are sufficient to reconstruct the real book.
struct ClientOrder {
    Side side;
    Price px;
    Quantity qty;
};

struct ClientBook {
    std::map<OrderId, ClientOrder> orders;
    uint64_t last_seq = 0;
    Quantity traded = 0;
    int trades = 0;

    void apply(const OutBoundEvent &ev) {
        std::visit(
            [&](const auto &e) {
                EXPECT(e.book_seq >= last_seq);
                last_seq = e.book_seq;
                using T = std::decay_t<decltype(e)>;
                if constexpr (std::is_same_v<T, OrderBookEvent>) {
                    switch (e.action) {
                    case BookAction::Add:
                        EXPECT(!orders.count(e.order_id));
                        orders[e.order_id] = {e.side, e.price, e.qty};
                        break;
                    case BookAction::Replace:
                        orders[e.order_id] = {e.side, e.price, e.qty};
                        break;
                    case BookAction::Modify:
                    case BookAction::Execute:
                        REQUIRE(orders.count(e.order_id));
                        EXPECT(e.qty < orders[e.order_id].qty);
                        orders[e.order_id].qty = e.qty;
                        break;
                    case BookAction::Delete:
                        EXPECT(orders.count(e.order_id));
                        orders.erase(e.order_id);
                        break;
                    }
                } else {
                    traded += e.qty;
                    trades++;
                }
            },
            ev);
    }

    void apply_all(Exchange &ex) {
        for (auto &e : drain(ex))
            apply(e);
    }
};
