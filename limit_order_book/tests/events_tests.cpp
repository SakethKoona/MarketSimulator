// Event-stream contract: a client holding only the events can reconstruct
// the book, and each mutation is published with the right action.
#include "test_helpers.hpp"

static std::vector<BookAction> actions(const std::vector<OutBoundEvent> &evs) {
    std::vector<BookAction> out;
    for (auto &e : evs)
        if (auto *b = std::get_if<OrderBookEvent>(&e))
            out.push_back(b->action);
    return out;
}

static int count_trades(const std::vector<OutBoundEvent> &evs) {
    int n = 0;
    for (auto &e : evs)
        n += std::holds_alternative<TradeFillEvent>(e);
    return n;
}

TEST(events_add_on_rest) {
    Exchange ex = make_exchange();
    auto r = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    auto evs = drain(ex);
    REQUIRE(evs.size() == 1u);
    auto *b = std::get_if<OrderBookEvent>(&evs[0]);
    REQUIRE(b != nullptr);
    EXPECT_EQ((int)b->action, (int)BookAction::Add);
    EXPECT_EQ(b->order_id, r.order_id);
    EXPECT_EQ(b->price, 100u);
    EXPECT_EQ(b->qty, 5u);
    EXPECT_EQ((int)b->side, (int)Side::Buy);
    EXPECT_EQ(b->symbol_id, 0u);
}

TEST(events_trade_pairs_with_execute_or_delete) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 100, 6, Side::Sell);
    ex.SubmitOrder("AAPL", 100, 2, Side::Sell);
    drain(ex);
    ex.SubmitOrder("AAPL", 100, 7, Side::Buy);
    auto evs = drain(ex);
    EXPECT_EQ(count_trades(evs), 2);
    auto acts = actions(evs);
    REQUIRE(acts.size() == 2u);
    EXPECT_EQ((int)acts[0], (int)BookAction::Delete);  // 6 fully consumed
    EXPECT_EQ((int)acts[1], (int)BookAction::Execute); // 2 -> 1

    // Each trade shares book_seq with the book event just before it
    for (std::size_t i = 1; i < evs.size(); i++) {
        if (auto *t = std::get_if<TradeFillEvent>(&evs[i])) {
            auto *b = std::get_if<OrderBookEvent>(&evs[i - 1]);
            REQUIRE(b != nullptr);
            EXPECT_EQ(t->book_seq, b->book_seq);
            EXPECT_EQ(t->resting_id, b->order_id);
            EXPECT_EQ((int)t->aggressor_side, (int)Side::Buy);
        }
    }
}

TEST(events_modify_cancel_replace) {
    Exchange ex = make_exchange();
    auto r = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    drain(ex);

    ex.ModifyOrder(r.order_id, 3);
    auto a = actions(drain(ex));
    REQUIRE(a.size() == 1u);
    EXPECT_EQ((int)a[0], (int)BookAction::Modify);

    ex.ModifyOrder(r.order_id, 3, 101);
    a = actions(drain(ex));
    REQUIRE(a.size() == 1u);
    EXPECT_EQ((int)a[0], (int)BookAction::Replace);

    ex.CancelOrder(r.order_id);
    a = actions(drain(ex));
    REQUIRE(a.size() == 1u);
    EXPECT_EQ((int)a[0], (int)BookAction::Delete);
}

TEST(events_replace_fully_filled_publishes_delete) {
    Exchange ex = make_exchange();
    ClientBook cb;
    ex.SubmitOrder("AAPL", 110, 5, Side::Sell);
    auto b = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    cb.apply_all(ex);
    EXPECT_EQ(cb.orders.size(), 2u);

    ex.ModifyOrder(b.order_id, 5, 110);
    auto evs = drain(ex);
    EXPECT_EQ(count_trades(evs), 1);
    auto a = actions(evs);
    REQUIRE(a.size() == 2u);
    EXPECT_EQ((int)a[0], (int)BookAction::Delete); // resting ask consumed
    EXPECT_EQ((int)a[1], (int)BookAction::Delete); // replaced bid fully filled
    for (auto &e : evs)
        cb.apply(e);
    EXPECT(cb.orders.empty());
}

TEST(events_sequence_is_monotonic) {
    Exchange ex = make_exchange();
    for (int i = 0; i < 50; i++)
        ex.SubmitOrder("AAPL", 100 + (i % 5), 1 + i % 3, i % 2 ? Side::Buy : Side::Sell);
    uint64_t last = 0;
    for (auto &e : drain(ex)) {
        uint64_t seq = std::visit([](auto &x) { return x.book_seq; }, e);
        EXPECT(seq >= last);
        last = seq;
    }
}

TEST(events_reconstruct_random_flow) {
    // Deterministic pseudo-random order flow; client book must match engine.
    Exchange ex = make_exchange(1 << 16);
    ClientBook cb;
    std::vector<OrderId> live;
    uint64_t x = 12345;
    auto rnd = [&] { x = x * 6364136223846793005ULL + 1442695040888963407ULL; return x >> 33; };

    for (int i = 0; i < 2000; i++) {
        int op = rnd() % 10;
        if (op < 6 || live.empty()) {
            Price px = 90 + rnd() % 21;
            Quantity qty = 1 + rnd() % 20;
            Side side = rnd() % 2 ? Side::Buy : Side::Sell;
            auto r = ex.SubmitOrder("AAPL", px, qty, side);
            if (r.resting)
                live.push_back(r.order_id);
        } else if (op < 8) {
            OrderId id = live[rnd() % live.size()];
            ex.CancelOrder(id);
        } else {
            OrderId id = live[rnd() % live.size()];
            ex.ModifyOrder(id, 1 + rnd() % 20, 90 + rnd() % 21);
        }
        cb.apply_all(ex);

        // Compare with the engine's own view via cancel probes is intrusive;
        // instead check the client book never crosses, which the engine
        // guarantees for its own book.
        Price best_bid = 0, best_ask = UINT64_MAX;
        for (auto &[id, o] : cb.orders) {
            if (o.side == Side::Buy) best_bid = std::max(best_bid, o.px);
            else best_ask = std::min(best_ask, o.px);
        }
        EXPECT(best_bid < best_ask);
    }

    // Every order the client still holds must be cancellable in the engine,
    // and every cancel must show up as a Delete.
    for (auto &[id, o] : std::map<OrderId, ClientOrder>(cb.orders)) {
        EXPECT_EQ((int)ex.CancelOrder(id), (int)StatusCode::Success);
        cb.apply_all(ex);
        EXPECT(!cb.orders.count(id));
    }
    EXPECT(cb.orders.empty());
    EXPECT(cb.trades > 0);
}
