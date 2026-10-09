// Matching-engine behaviour through the Exchange API.
#include "test_helpers.hpp"

TEST(engine_rests_non_crossing_orders) {
    Exchange ex = make_exchange();
    auto r = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    EXPECT_EQ((int)r.fill_status, (int)FillStatus::Accepted);
    EXPECT_EQ((int)r.status_code, (int)StatusCode::Success);
    EXPECT_EQ(r.qty_executed, 0u);
    EXPECT_EQ(r.qty_remaining, 5u);
    EXPECT(r.resting);
    EXPECT_EQ(r.order_id, 1u); // ids start at 1; 0 is the sentinel
    auto s = ex.SubmitOrder("AAPL", 101, 5, Side::Sell);
    EXPECT_EQ(s.qty_executed, 0u);
    EXPECT_EQ(s.order_id, 2u);
}

TEST(engine_full_fill_at_resting_price) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 100, 10, Side::Sell);
    auto r = ex.SubmitOrder("AAPL", 105, 10, Side::Buy);
    EXPECT_EQ((int)r.fill_status, (int)FillStatus::FullyFilled);
    EXPECT_EQ(r.qty_executed, 10u);
    EXPECT_EQ(r.qty_remaining, 0u);
    auto evs = drain(ex);
    bool saw_trade = false;
    for (auto &e : evs)
        if (auto *t = std::get_if<TradeFillEvent>(&e)) {
            saw_trade = true;
            EXPECT_EQ(t->price, 100u); // trades at resting price
            EXPECT_EQ(t->qty, 10u);
        }
    EXPECT(saw_trade);
}

TEST(engine_partial_fill_shrinks_resting) {
    Exchange ex = make_exchange();
    auto rest = ex.SubmitOrder("AAPL", 100, 10, Side::Sell);
    auto r = ex.SubmitOrder("AAPL", 100, 4, Side::Buy);
    EXPECT_EQ((int)r.fill_status, (int)FillStatus::FullyFilled);
    EXPECT_EQ(r.qty_executed, 4u);
    ClientBook cb;
    cb.apply_all(ex);
    REQUIRE(cb.orders.count(rest.order_id));
    EXPECT_EQ(cb.orders[rest.order_id].qty, 6u);
}

TEST(engine_sweeps_multiple_levels_and_rests_remainder) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 50, 2, Side::Sell);
    ex.SubmitOrder("AAPL", 51, 2, Side::Sell);
    ex.SubmitOrder("AAPL", 60, 2, Side::Sell); // above limit, untouched
    auto r = ex.SubmitOrder("AAPL", 55, 5, Side::Buy);
    EXPECT_EQ((int)r.fill_status, (int)FillStatus::PartiallyFilled);
    EXPECT_EQ(r.qty_executed, 4u);
    EXPECT_EQ(r.qty_remaining, 1u);
    EXPECT(r.resting);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT_EQ(cb.orders.size(), 2u); // 2@60 ask and 1@55 bid
    REQUIRE(cb.orders.count(r.order_id));
    EXPECT_EQ(cb.orders[r.order_id].qty, 1u);
    EXPECT_EQ(cb.orders[r.order_id].px, 55u);
}

TEST(engine_time_priority_within_level) {
    Exchange ex = make_exchange();
    auto a = ex.SubmitOrder("AAPL", 100, 5, Side::Sell);
    auto b = ex.SubmitOrder("AAPL", 100, 5, Side::Sell);
    ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT(!cb.orders.count(a.order_id)); // first in, first filled
    EXPECT(cb.orders.count(b.order_id));
}

TEST(engine_market_order) {
    Exchange ex = make_exchange();
    auto empty = ex.SubmitOrder("AAPL", 0, 5, Side::Buy, OrderType::MARKET);
    EXPECT_EQ((int)empty.fill_status, (int)FillStatus::Rejected);
    EXPECT_EQ(empty.qty_remaining, 5u);
    EXPECT(drain(ex).empty()); // nothing rested, nothing published

    ex.SubmitOrder("AAPL", 100, 3, Side::Sell);
    ex.SubmitOrder("AAPL", 200, 3, Side::Sell);
    auto m = ex.SubmitOrder("AAPL", 0, 5, Side::Buy, OrderType::MARKET);
    EXPECT_EQ((int)m.fill_status, (int)FillStatus::FullyFilled);
    EXPECT_EQ(m.qty_executed, 5u);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT_EQ(cb.orders.size(), 1u);
    EXPECT_EQ(cb.traded, 5u);
}

TEST(engine_ioc_does_not_rest) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 100, 3, Side::Sell);
    auto r = ex.SubmitOrder("AAPL", 100, 10, Side::Buy, OrderType::LIMIT, TypeInForce::IOC);
    EXPECT_EQ((int)r.fill_status, (int)FillStatus::PartiallyFilled);
    EXPECT_EQ(r.qty_executed, 3u);
    EXPECT_EQ(r.qty_remaining, 7u);
    EXPECT(!r.resting);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT(cb.orders.empty());
}

TEST(engine_fok_all_or_nothing) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 100, 3, Side::Sell);
    ex.SubmitOrder("AAPL", 101, 3, Side::Sell);
    drain(ex);

    auto no = ex.SubmitOrder("AAPL", 100, 5, Side::Buy, OrderType::LIMIT, TypeInForce::FOK);
    EXPECT_EQ((int)no.status_code, (int)StatusCode::FOKFailed);
    EXPECT_EQ(no.qty_executed, 0u);
    EXPECT(drain(ex).empty()); // book untouched

    auto yes = ex.SubmitOrder("AAPL", 101, 5, Side::Buy, OrderType::LIMIT, TypeInForce::FOK);
    EXPECT_EQ((int)yes.fill_status, (int)FillStatus::FullyFilled);
    EXPECT_EQ(yes.qty_executed, 5u);
}

TEST(engine_cancel) {
    Exchange ex = make_exchange();
    auto r = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    EXPECT_EQ((int)ex.CancelOrder(r.order_id), (int)StatusCode::Success);
    EXPECT_EQ((int)ex.CancelOrder(r.order_id), (int)StatusCode::OrderNotFound);
    EXPECT_EQ((int)ex.CancelOrder(12345), (int)StatusCode::OrderNotFound);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT(cb.orders.empty());
}

TEST(engine_cancel_after_fill_is_not_found) {
    Exchange ex = make_exchange();
    auto r = ex.SubmitOrder("AAPL", 100, 5, Side::Sell);
    ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    EXPECT_EQ((int)ex.CancelOrder(r.order_id), (int)StatusCode::OrderNotFound);
}

TEST(engine_modify_qty_down_keeps_priority) {
    Exchange ex = make_exchange();
    auto a = ex.SubmitOrder("AAPL", 100, 10, Side::Sell);
    auto b = ex.SubmitOrder("AAPL", 100, 10, Side::Sell);
    EXPECT_EQ((int)ex.ModifyOrder(a.order_id, 2), (int)StatusCode::Success);
    ex.SubmitOrder("AAPL", 100, 2, Side::Buy);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT(!cb.orders.count(a.order_id)); // a still ahead of b
    EXPECT_EQ(cb.orders[b.order_id].qty, 10u);
}

TEST(engine_modify_qty_up_loses_priority) {
    Exchange ex = make_exchange();
    auto a = ex.SubmitOrder("AAPL", 100, 5, Side::Sell);
    auto b = ex.SubmitOrder("AAPL", 100, 5, Side::Sell);
    EXPECT_EQ((int)ex.ModifyOrder(a.order_id, 8), (int)StatusCode::Success);
    ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT(!cb.orders.count(b.order_id)); // b now ahead of a
    EXPECT_EQ(cb.orders[a.order_id].qty, 8u);
}

TEST(engine_modify_price_crossing_matches) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 110, 3, Side::Sell);
    auto b = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    EXPECT_EQ((int)ex.ModifyOrder(b.order_id, 5, 110), (int)StatusCode::Success);
    ClientBook cb;
    cb.apply_all(ex);
    EXPECT_EQ(cb.traded, 3u);
    EXPECT_EQ(cb.orders.size(), 1u);
    REQUIRE(cb.orders.count(b.order_id));
    EXPECT_EQ(cb.orders[b.order_id].qty, 2u);
    EXPECT_EQ(cb.orders[b.order_id].px, 110u);
}

TEST(engine_modify_to_zero_cancels) {
    Exchange ex = make_exchange();
    auto r = ex.SubmitOrder("AAPL", 100, 5, Side::Buy);
    drain(ex);
    EXPECT_EQ((int)ex.ModifyOrder(r.order_id, 0), (int)StatusCode::Success);
    auto evs = drain(ex);
    REQUIRE(evs.size() == 1u);
    auto *b = std::get_if<OrderBookEvent>(&evs[0]);
    REQUIRE(b != nullptr);
    EXPECT_EQ((int)b->action, (int)BookAction::Delete); // not Modify qty=0
    EXPECT_EQ((int)ex.ModifyOrder(r.order_id, 3), (int)StatusCode::OrderNotFound);
    EXPECT_EQ((int)ex.CancelOrder(r.order_id), (int)StatusCode::OrderNotFound);
}

TEST(engine_rejects_invalid_orders) {
    Exchange ex = make_exchange();
    auto q = ex.SubmitOrder("AAPL", 100, 0, Side::Buy);
    EXPECT_EQ((int)q.fill_status, (int)FillStatus::Rejected);
    EXPECT_EQ((int)q.status_code, (int)StatusCode::InvalidQuantity);
    auto p = ex.SubmitOrder("AAPL", 0, 5, Side::Buy);
    EXPECT_EQ((int)p.fill_status, (int)FillStatus::Rejected);
    EXPECT_EQ((int)p.status_code, (int)StatusCode::InvalidPrice);
    // market orders carry no price, so 0 is fine there
    auto m = ex.SubmitOrder("AAPL", 0, 5, Side::Buy, OrderType::MARKET);
    EXPECT_EQ((int)m.status_code, (int)StatusCode::Success);
    EXPECT(drain(ex).empty());
}

TEST(engine_sequences_are_per_instance) {
    Exchange a = make_exchange();
    Exchange b = make_exchange();
    auto ra = a.SubmitOrder("AAPL", 100, 5, Side::Buy);
    auto rb = b.SubmitOrder("AAPL", 100, 5, Side::Buy);
    EXPECT_EQ(ra.order_id, rb.order_id);
    auto ea = drain(a), eb = drain(b);
    REQUIRE(ea.size() == 1u && eb.size() == 1u);
    EXPECT_EQ(std::get<OrderBookEvent>(ea[0]).book_seq,
              std::get<OrderBookEvent>(eb[0]).book_seq);
}

TEST(engine_symbols_are_isolated) {
    Exchange ex = make_exchange();
    ex.SubmitOrder("AAPL", 100, 5, Side::Sell);
    auto r = ex.SubmitOrder("GOOG", 100, 5, Side::Buy);
    EXPECT_EQ(r.qty_executed, 0u);

    // Unknown symbols are a status, never an exception
    auto bad = ex.SubmitOrder("MSFT", 100, 5, Side::Buy);
    EXPECT_EQ((int)bad.fill_status, (int)FillStatus::Rejected);
    EXPECT_EQ((int)bad.status_code, (int)StatusCode::SymbolNotFound);
    EXPECT_EQ((int)ex.L2Snapshot("MSFT"), (int)StatusCode::SymbolNotFound);
    EXPECT_EQ((int)ex.DisplayBook("MSFT"), (int)StatusCode::SymbolNotFound);
    auto bad_id = ex.SubmitOrder(SymbolId{99}, 100, 5, Side::Buy);
    EXPECT_EQ((int)bad_id.status_code, (int)StatusCode::SymbolNotFound);

    // SymbolId overload reaches the same book as the name
    auto id = ex.ResolveSymbol("GOOG");
    REQUIRE(id.has_value());
    auto s = ex.SubmitOrder(*id, 100, 5, Side::Sell);
    EXPECT_EQ(s.qty_executed, 5u); // matched the GOOG bid above
    EXPECT(!ex.ResolveSymbol("MSFT").has_value());
}
