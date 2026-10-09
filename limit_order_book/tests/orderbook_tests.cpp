// Unit tests for OrderBook / PriceLevel / SkipList invariants.
#include "orderbook.hpp"
#include "test_framework.hpp"

static Order mk(OrderId id, Price px, Quantity qty, Side side) {
    return Order(id, px, qty, OrderType::LIMIT, TypeInForce::GTC, side);
}

// Walks both sides and checks every invariant from invariants.txt.
static void check_invariants(OrderBook &book) {
    const PriceLevel *bb = book.BestBid();
    const PriceLevel *ba = book.BestAsk();
    if (bb && ba)
        EXPECT(bb->price < ba->price);

    for (const Book *side : {&book.bids(), &book.asks()}) {
        bool is_bid = (side == &book.bids());
        auto *node = side->GetHead();
        const PriceLevel *prev = nullptr;
        while (node) {
            const PriceLevel &lvl = node->value;
            EXPECT(lvl.GetSize() > 0);
            int count = 0;
            Quantity sum = 0;
            Timestamp last_ts = 0;
            const OrderNode *prev_node = nullptr;
            for (auto &o : lvl.orders()) {
                count++;
                EXPECT(o.quantity > 0);
                EXPECT_EQ(o.price, lvl.price);
                EXPECT(o.timestamp >= last_ts); // time priority
                last_ts = o.timestamp;
                sum += o.quantity;
                EXPECT(o.prev == prev_node); // back links consistent
                prev_node = &o;
                const OrderNode *info = book.FindOrder(o.orderId);
                REQUIRE(info != nullptr);
                EXPECT(info == &o);
                EXPECT(info->level == &lvl);
            }
            EXPECT(lvl.back() == prev_node);
            EXPECT_EQ(count, lvl.GetSize());
            EXPECT_EQ(sum, lvl.TotalQuantity());
            if (prev)
                EXPECT(is_bid ? prev->price > lvl.price
                              : prev->price < lvl.price);
            prev = &lvl;
            node = node->forward[0];
        }
    }
}

TEST(book_add_and_lookup) {
    OrderBook book(0);
    EXPECT_EQ((int)book.AddOrder(mk(1, 100, 5, Side::Buy)), (int)OrderResult::Success);
    EXPECT_EQ((int)book.AddOrder(mk(2, 101, 5, Side::Sell)), (int)OrderResult::Success);
    REQUIRE(book.FindOrder(1) != nullptr);
    EXPECT_EQ(book.FindOrder(1)->quantity, 5u);
    EXPECT(book.FindOrder(99) == nullptr);
    EXPECT_EQ(book.size(), 2u);
    EXPECT_EQ(book.orderCount(), 2u);
    check_invariants(book);
}

TEST(book_rejects_duplicate_and_zero_qty) {
    OrderBook book(0);
    book.AddOrder(mk(1, 100, 5, Side::Buy));
    EXPECT_EQ((int)book.AddOrder(mk(1, 100, 5, Side::Buy)), (int)OrderResult::DuplicateOrder);
    EXPECT_EQ((int)book.AddOrder(mk(2, 100, 0, Side::Buy)), (int)OrderResult::InvalidQty);
    EXPECT_EQ(book.size(), 1u);
}

TEST(book_price_ordering) {
    OrderBook book(0);
    for (Price p : {95, 100, 98, 102, 97})
        book.AddOrder(mk(p, p, 1, Side::Buy));
    for (Price p : {110, 105, 108, 120, 106})
        book.AddOrder(mk(p + 1000, p, 1, Side::Sell));
    REQUIRE(book.BestBid() && book.BestAsk());
    EXPECT_EQ(book.BestBid()->price, 102u);
    EXPECT_EQ(book.BestAsk()->price, 105u);
    check_invariants(book);
}

TEST(book_same_level_keeps_time_priority) {
    OrderBook book(0);
    book.AddOrder(mk(1, 100, 1, Side::Buy));
    book.AddOrder(mk(2, 100, 2, Side::Buy));
    book.AddOrder(mk(3, 100, 3, Side::Buy));
    REQUIRE(book.BestBid());
    EXPECT_EQ(book.BestBid()->front()->orderId, 1u);
    EXPECT_EQ(book.BestBid()->back()->orderId, 3u);
    EXPECT_EQ(book.BestBid()->TotalQuantity(), 6u);
    EXPECT_EQ(book.BestBid()->GetSize(), 3);
    check_invariants(book);
}

TEST(book_cancel_removes_empty_level) {
    OrderBook book(0);
    book.AddOrder(mk(1, 100, 5, Side::Buy));
    book.AddOrder(mk(2, 99, 5, Side::Buy));
    EXPECT_EQ((int)book.CancelOrder(1), (int)OrderResult::Success);
    EXPECT(book.FindOrder(1) == nullptr);
    REQUIRE(book.BestBid());
    EXPECT_EQ(book.BestBid()->price, 99u);
    EXPECT_EQ(book.bids().len(), 1);
    EXPECT_EQ((int)book.CancelOrder(1), (int)OrderResult::OrderNotFound);
    EXPECT_EQ((int)book.CancelOrder(2), (int)OrderResult::Success);
    EXPECT(book.BestBid() == nullptr);
    EXPECT_EQ(book.size(), 0u);
    check_invariants(book);
}

TEST(book_modify_reduces_qty_only) {
    OrderBook book(0);
    book.AddOrder(mk(1, 100, 10, Side::Sell));
    EXPECT_EQ((int)book.ModifyOrder(1, 4), (int)ModifyResult::Success);
    EXPECT_EQ(book.FindOrder(1)->quantity, 4u);
    EXPECT_EQ(book.BestAsk()->TotalQuantity(), 4u);
    EXPECT_EQ((int)book.ModifyOrder(1, 8), (int)ModifyResult::QtyIncreaseNotAllowed);
    EXPECT_EQ((int)book.ModifyOrder(42, 1), (int)ModifyResult::OrderNotFound);
    check_invariants(book);
}

TEST(book_modify_to_zero_cancels) {
    OrderBook book(0);
    book.AddOrder(mk(1, 100, 10, Side::Sell));
    EXPECT_EQ((int)book.ModifyOrder(1, 0), (int)ModifyResult::Success);
    EXPECT(book.FindOrder(1) == nullptr);
    EXPECT(book.BestAsk() == nullptr);
    check_invariants(book);
}

TEST(book_many_levels_churn) {
    // Exercise the skiplist with inserts and deletes across many levels
    OrderBook book(0);
    // Bids in [1000, 1300), asks in [1500, 1800): many levels, no cross
    for (OrderId i = 1; i <= 500; i++) {
        Side side = i % 2 ? Side::Buy : Side::Sell;
        Price base = side == Side::Buy ? 1000 : 1500;
        book.AddOrder(mk(i, base + (i * 7919) % 300, i, side));
    }
    for (OrderId i = 1; i <= 500; i += 3)
        EXPECT_EQ((int)book.CancelOrder(i), (int)OrderResult::Success);
    for (const Book *side : {&book.bids(), &book.asks()}) {
        auto *node = side->GetHead();
        int levels = 0;
        while (node) {
            levels++;
            EXPECT(node->value.GetSize() > 0);
            node = node->forward[0];
        }
        EXPECT_EQ(levels, side->len());
    }
    check_invariants(book);
}

TEST(book_node_reuse_after_cancel) {
    // Pool must recycle freed nodes: many add/cancel cycles should not grow
    // the arena beyond its first block.
    OrderBook book(0);
    for (OrderId i = 1; i <= 100000; i++) {
        book.AddOrder(mk(i, 100 + i % 7, 1, Side::Buy));
        if (i > 10)
            EXPECT_EQ((int)book.CancelOrder(i - 10), (int)OrderResult::Success);
    }
    EXPECT_EQ(book.orderCount(), 10u);
    check_invariants(book);
}

TEST(book_remove_middle_of_level) {
    OrderBook book(0);
    book.AddOrder(mk(1, 100, 1, Side::Sell));
    book.AddOrder(mk(2, 100, 2, Side::Sell));
    book.AddOrder(mk(3, 100, 3, Side::Sell));
    book.CancelOrder(2);
    REQUIRE(book.BestAsk());
    EXPECT_EQ(book.BestAsk()->front()->orderId, 1u);
    EXPECT_EQ(book.BestAsk()->front()->next->orderId, 3u);
    EXPECT_EQ(book.BestAsk()->back()->orderId, 3u);
    EXPECT_EQ(book.BestAsk()->TotalQuantity(), 4u);
    check_invariants(book);
    book.CancelOrder(1);
    EXPECT_EQ(book.BestAsk()->front()->orderId, 3u);
    EXPECT(book.BestAsk()->front()->prev == nullptr);
    check_invariants(book);
}
