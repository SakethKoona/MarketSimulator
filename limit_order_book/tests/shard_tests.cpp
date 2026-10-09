// Sharding: symbols spread over N engines, ids tagged with their shard,
// routing by SymbolId and OrderId, and per-shard event sinks.
#include "test_helpers.hpp"
#include <set>

static const std::vector<std::string> kFive = {"AAPL", "GOOG", "NVDA", "MSFT",
                                               "AMZN"};

TEST(shard_id_helpers) {
    OrderId id = make_order_id(7, 12345);
    EXPECT_EQ((int)shard_of(id), 7);
    EXPECT_EQ(seq_of(id), 12345u);
    EXPECT_EQ(make_order_id(0, 1), 1u); // shard 0 ids are plain sequences
    EXPECT(make_order_id(1, 1) != make_order_id(0, 1));
    EXPECT_EQ(seq_of(make_shard_id(255, kSeqMask)), kSeqMask);
}

TEST(shard_symbol_assignment) {
    Exchange ex = make_exchange(4096, 3, kFive);
    EXPECT_EQ(ex.NumShards(), 3u);
    for (SymbolId s = 0; s < 5; s++) {
        EXPECT_EQ((int)ex.ShardOf(s), (int)(s % 3));
        EXPECT(ex.Shard(s % 3).Serves(s));
        for (std::size_t other = 0; other < 3; other++)
            if (other != s % 3)
                EXPECT(!ex.Shard(other).Serves(s));
    }
}

TEST(shard_orders_tagged_and_routed) {
    Exchange ex = make_exchange(4096, 3, kFive);
    std::vector<OrderId> ids;
    for (SymbolId s = 0; s < 5; s++) {
        auto r = ex.SubmitOrder(s, 100, 10, Side::Buy);
        EXPECT_EQ((int)r.fill_status, (int)FillStatus::Accepted);
        EXPECT_EQ((int)shard_of(r.order_id), (int)ex.ShardOf(s));
        ids.push_back(r.order_id);
    }
    // Shards 0 and 1 each minted two ids (seq 1,2), shard 2 one (seq 1);
    // all five are distinct
    std::set<OrderId> uniq(ids.begin(), ids.end());
    EXPECT_EQ(uniq.size(), 5u);
    EXPECT_EQ(seq_of(ids[0]), 1u);
    EXPECT_EQ(seq_of(ids[3]), 2u); // MSFT is the second symbol on shard 0

    // Cancel / modify route by the shard bits
    EXPECT_EQ((int)ex.ModifyOrder(ids[4], 5), (int)StatusCode::Success);
    EXPECT_EQ(ex.GetBook(4).FindOrder(ids[4])->quantity, 5u);
    for (auto id : ids)
        EXPECT_EQ((int)ex.CancelOrder(id), (int)StatusCode::Success);
    for (auto id : ids)
        EXPECT_EQ((int)ex.CancelOrder(id), (int)StatusCode::OrderNotFound);

    // An id for a shard that doesn't exist is simply not found
    EXPECT_EQ((int)ex.CancelOrder(make_order_id(9, 1)), (int)StatusCode::OrderNotFound);
    // A well-formed id on the wrong shard is not found either
    EXPECT_EQ((int)ex.CancelOrder(make_order_id(1, 1)), (int)StatusCode::OrderNotFound);
}

TEST(shard_count_clamped_to_symbol_count) {
    // 8 shards for 2 symbols: each symbol still has exactly one owner
    Exchange ex = make_exchange(4096, 8, {"AAPL", "GOOG"});
    EXPECT_EQ(ex.NumShards(), 2u);
    EXPECT_EQ((int)ex.ShardOf(0), 0);
    EXPECT_EQ((int)ex.ShardOf(1), 1);
    auto r = ex.SubmitOrder(SymbolId{1}, 100, 5, Side::Buy);
    EXPECT_EQ((int)shard_of(r.order_id), 1);
    EXPECT_EQ((int)ex.CancelOrder(r.order_id), (int)StatusCode::Success);
}

TEST(shard_matching_stays_within_symbol) {
    Exchange ex = make_exchange(4096, 2, kFive);
    ex.SubmitOrder(SymbolId{0}, 100, 10, Side::Sell); // shard 0
    ex.SubmitOrder(SymbolId{1}, 100, 10, Side::Sell); // shard 1
    auto r = ex.SubmitOrder(SymbolId{2}, 100, 10, Side::Buy); // shard 0, no match
    EXPECT_EQ(r.qty_executed, 0u);
    auto m = ex.SubmitOrder(SymbolId{0}, 100, 10, Side::Buy);
    EXPECT_EQ(m.qty_executed, 10u);
}

TEST(shard_sinks_are_separate_and_ids_unique) {
    Exchange ex = make_exchange(4096, 3, kFive);
    // Two fills on different shards
    ex.SubmitOrder(SymbolId{0}, 100, 5, Side::Sell);
    ex.SubmitOrder(SymbolId{0}, 100, 5, Side::Buy);
    ex.SubmitOrder(SymbolId{1}, 100, 5, Side::Sell);
    ex.SubmitOrder(SymbolId{1}, 100, 5, Side::Buy);
    ex.SubmitOrder(SymbolId{2}, 100, 5, Side::Buy); // shard 2, rests

    // Each shard's sink holds only its own symbols' events
    std::set<uint64_t> trade_ids, book_seqs;
    for (std::size_t s = 0; s < 3; s++) {
        int n = 0;
        while (auto *e = ex.Sink(s).consume()) {
            n++;
            std::visit([&](auto &ev) {
                EXPECT_EQ((int)ex.ShardOf(ev.symbol_id), (int)s);
                EXPECT_EQ((int)shard_of(ev.book_seq), (int)s);
                book_seqs.insert(ev.book_seq);
                if constexpr (std::is_same_v<std::decay_t<decltype(ev)>, TradeFillEvent>) {
                    EXPECT_EQ((int)shard_of(ev.trade_id), (int)s);
                    trade_ids.insert(ev.trade_id);
                }
            }, *e);
        }
        EXPECT(n > 0);
    }
    EXPECT_EQ(trade_ids.size(), 2u); // distinct across shards
    // shard0: Add + (Execute,Trade share one seq) = 2; shard1: 2; shard2: 1
    EXPECT_EQ(book_seqs.size(), 5u);
}

TEST(shard_next_event_round_robin_drains_all) {
    Exchange ex = make_exchange(4096, 3, kFive);
    for (SymbolId s = 0; s < 5; s++)
        ex.SubmitOrder(s, 100, 1, Side::Buy);
    int n = 0;
    while (ex.NextEvent())
        n++;
    EXPECT_EQ(n, 5);
    for (std::size_t s = 0; s < 3; s++)
        EXPECT(ex.Sink(s).consume() == nullptr);
}

TEST(shard_reconstruct_random_flow_three_shards) {
    Exchange ex = make_exchange(1 << 16, 3, kFive);
    std::map<SymbolId, ClientBook> books;
    std::vector<OrderId> live;
    uint64_t x = 777;
    auto rnd = [&] { x = x * 6364136223846793005ULL + 1442695040888963407ULL; return x >> 33; };

    auto drain_all = [&] {
        while (auto *e = ex.NextEvent()) {
            SymbolId sym = std::visit([](auto &ev) { return ev.symbol_id; }, *e);
            books[sym].apply(*e);
        }
    };

    for (int i = 0; i < 3000; i++) {
        SymbolId sym = rnd() % 5;
        int op = rnd() % 10;
        if (op < 6 || live.empty()) {
            auto r = ex.SubmitOrder(sym, 90 + rnd() % 21, 1 + rnd() % 20,
                                    rnd() % 2 ? Side::Buy : Side::Sell);
            if (r.resting)
                live.push_back(r.order_id);
        } else if (op < 8) {
            OrderId id = live[rnd() % live.size()];
            ex.CancelOrder(id);
        } else {
            OrderId id = live[rnd() % live.size()];
            ex.ModifyOrder(id, 1 + rnd() % 20, 90 + rnd() % 21);
        }
        drain_all();
    }

    // Client books match the engine's books, symbol by symbol
    for (SymbolId sym = 0; sym < 5; sym++) {
        const OrderBook &real = ex.GetBook(sym);
        ClientBook &cb = books[sym];
        EXPECT_EQ(cb.orders.size(), real.orderCount());
        for (auto &[id, o] : cb.orders) {
            const OrderNode *n = real.FindOrder(id);
            REQUIRE(n != nullptr);
            EXPECT_EQ(n->quantity, o.qty);
            EXPECT_EQ(n->price, o.px);
            EXPECT_EQ((int)shard_of(id), (int)ex.ShardOf(sym));
        }
    }
}
