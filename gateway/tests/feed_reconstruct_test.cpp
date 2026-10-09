// Conformance test: drive the engine with random flow, encode its events
// into wire messages with the same FeedFramer the publisher uses, decode
// those bytes into a client-side L3 book, and require it to equal the
// engine's book level by level and order by order.
#include "exchange.hpp"
#include "feed_framer.hpp"
#include "protocol/feed.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <random>
#include <unordered_map>

namespace {

struct COrder { std::uint32_t sym; std::uint8_t side; std::uint64_t px; std::uint32_t qty; };
struct CLevel { std::list<std::uint64_t> ids; std::uint64_t qty = 0; };

struct ClientBook {
    std::unordered_map<std::uint64_t, COrder> orders;
    // sym -> side -> price -> level
    std::map<std::uint32_t, std::map<std::uint8_t, std::map<std::uint64_t, CLevel>>> levels;
    std::uint64_t volume = 0, fills = 0;
    std::unordered_map<std::uint64_t, bool> match_ids;
    // Per shard: count of fills and the highest per-shard sequence seen.
    std::map<std::uint8_t, std::pair<std::uint64_t, std::uint64_t>> per_shard;

    void add(std::uint32_t sym, std::uint8_t side, std::uint64_t id, std::uint64_t px, std::uint32_t q) {
        assert(!orders.count(id) && "duplicate add");
        orders[id] = {sym, side, px, q};
        auto &lv = levels[sym][side][px];
        lv.ids.push_back(id);
        lv.qty += q;
    }
    void remove(std::uint64_t id) {
        auto it = orders.find(id);
        assert(it != orders.end() && "remove unknown");
        auto &lv = levels[it->second.sym][it->second.side][it->second.px];
        lv.qty -= it->second.qty;
        lv.ids.remove(id);
        if (lv.ids.empty())
            levels[it->second.sym][it->second.side].erase(it->second.px);
        orders.erase(it);
    }
    void shrink(std::uint64_t id, std::uint32_t remaining) {
        auto it = orders.find(id);
        assert(it != orders.end() && "shrink unknown");
        assert(remaining < it->second.qty && "shrink must reduce");
        auto &lv = levels[it->second.sym][it->second.side][it->second.px];
        lv.qty -= (it->second.qty - remaining);
        it->second.qty = remaining;
    }

    template <typename T> static T ld(const char *p) { T t; std::memcpy(&t, p, sizeof(T)); return t; }

    void apply(const char *p, std::uint16_t len) {
        using namespace feed;
        switch (static_cast<MsgType>(static_cast<std::uint8_t>(p[0]))) {
        case MsgType::AddOrder: {
            assert(len == sizeof(AddOrder));
            auto m = ld<AddOrder>(p);
            add(m.symbol_id, m.side, m.order_id, m.price, m.qty);
            break;
        }
        case MsgType::OrderExecuted: {
            assert(len == sizeof(OrderExecuted));
            auto m = ld<OrderExecuted>(p);
            auto it = orders.find(m.order_id);
            assert(it != orders.end() && "exec unknown");
            assert(it->second.px == m.price && "exec price != resting price");
            assert(it->second.side == m.side);
            assert(it->second.qty == m.exec_qty + m.remaining_qty && "exec qty mismatch");
            assert(m.match_id != 0);
            volume += m.exec_qty; ++fills;
            assert(!match_ids.count(m.match_id) && "duplicate match id");
            match_ids[m.match_id] = true;
            auto &ps = per_shard[shard_of(m.match_id)];
            ps.first++;
            if (seq_of(m.match_id) > ps.second) ps.second = seq_of(m.match_id);
            if (m.remaining_qty == 0) remove(m.order_id); else shrink(m.order_id, m.remaining_qty);
            break;
        }
        case MsgType::OrderCancel: {
            assert(len == sizeof(OrderCancel));
            auto m = ld<OrderCancel>(p);
            assert(m.remaining_qty > 0);
            shrink(m.order_id, m.remaining_qty);
            break;
        }
        case MsgType::OrderDelete: {
            assert(len == sizeof(OrderDelete));
            auto m = ld<OrderDelete>(p);
            remove(m.order_id);
            break;
        }
        case MsgType::OrderReplace: {
            assert(len == sizeof(OrderReplace));
            auto m = ld<OrderReplace>(p);
            // Same id, new price/qty, back of the queue.
            auto it = orders.find(m.order_id);
            if (it != orders.end()) remove(m.order_id);
            add(m.symbol_id, m.side, m.order_id, m.price, m.qty);
            break;
        }
        default:
            assert(false && "unexpected message type");
        }
    }
};

std::uint8_t wire(Side s) { return s == Side::Buy ? 'B' : 'S'; }

// Compare one engine side (skiplist of PriceLevel) with the client's map.
void compare_side(const Book &eng, const std::map<std::uint64_t, CLevel> &cli,
                  std::uint32_t sym, const char *name) {
    std::size_t n = 0;
    for (auto *node = eng.GetHead(); node; node = node->forward[0]) {
        const PriceLevel &pl = node->value;
        auto it = cli.find(pl.price);
        if (it == cli.end()) {
            std::fprintf(stderr, "sym %u %s: engine level %llu missing on client\n", sym, name,
                         (unsigned long long)pl.price);
            assert(false);
        }
        if (it->second.qty != pl.TotalQuantity() || it->second.ids.size() != (std::size_t)pl.GetSize()) {
            std::fprintf(stderr, "sym %u %s px %llu: engine qty=%llu n=%d client qty=%llu n=%zu\n",
                         sym, name, (unsigned long long)pl.price,
                         (unsigned long long)pl.TotalQuantity(), pl.GetSize(),
                         (unsigned long long)it->second.qty, it->second.ids.size());
            assert(false);
        }
        auto cid = it->second.ids.begin();
        for (const OrderNode &o : pl.orders()) {
            assert(*cid == o.orderId && "time priority order differs");
            ++cid;
        }
        ++n;
    }
    assert(n == cli.size() && "client has extra levels");
}

} // namespace

int main(int argc, char **argv) {
    const int ops = argc > 1 ? std::atoi(argv[1]) : 200000;
    const int shards = argc > 2 ? std::atoi(argv[2]) : 1;
    json cfg = {{"symbols", {{"AAA", json::object()}, {"BBB", json::object()}, {"CCC", json::object()}}},
                {"sink_size", 1 << 16}, {"seq_capacity", 10}, {"num_symbols", 3},
                {"shards", shards}};
    Exchange ex(cfg);
    // One framer per shard sink, exactly as the publisher does it.
    std::vector<FeedFramer> framers;
    for (std::size_t i = 0; i < ex.NumShards(); ++i)
        framers.emplace_back(ex.Sink(i));
    ClientBook client;
    char buf[feed::kMaxMessageSize];
    auto drain = [&] {
        for (auto &framer : framers)
            while (std::uint16_t n = framer.next(buf, 0))
                client.apply(buf, n);
    };
    auto unpaired = [&] {
        std::uint64_t u = 0;
        for (auto &f : framers) u += f.unpaired();
        return u;
    };

    std::mt19937_64 rng(7);
    std::vector<SymbolId> syms;
    for (auto &[name, id] : ex.Symbols()) syms.push_back(id);
    std::vector<std::uint64_t> mid(syms.size(), 1000);
    std::vector<OrderId> live;
    std::uint64_t engine_volume = 0, msgs = 0;

    for (int i = 0; i < ops; ++i) {
        std::size_t si = rng() % syms.size();
        SymbolId sym = syms[si];
        if (rng() % 10 == 0) mid[si] += (rng() % 3) - 1;
        unsigned r = rng() % 100;
        if (r < 50) {
            Side side = (rng() % 2) ? Side::Buy : Side::Sell;
            std::uint64_t off = rng() % 8; // 0 crosses often
            Price px = side == Side::Buy ? mid[si] - off + 2 : mid[si] + off - 2;
            TypeInForce tif = (rng() % 10 == 0) ? TypeInForce::IOC : (rng() % 15 == 0) ? TypeInForce::FOK : TypeInForce::GTC;
            auto res = ex.SubmitOrder(sym, px, 1 + rng() % 100, side, OrderType::LIMIT, tif);
            engine_volume += res.qty_executed;
            if (res.resting) live.push_back(res.order_id);
        } else if (r < 58) {
            Side side = (rng() % 2) ? Side::Buy : Side::Sell;
            auto res = ex.SubmitOrder(sym, 0, 1 + rng() % 150, side, OrderType::MARKET, TypeInForce::IOC);
            engine_volume += res.qty_executed;
        } else if (r < 78 && !live.empty()) {
            std::size_t k = rng() % live.size();
            ex.CancelOrder(live[k]);
            live[k] = live.back(); live.pop_back();
        } else if (r < 90 && !live.empty()) {
            std::size_t k = rng() % live.size();
            ex.ModifyOrder(live[k], 1 + rng() % 60); // may shrink, may replace (increase)
        } else if (!live.empty()) {
            std::size_t k = rng() % live.size();
            // price change: cancel-replace that may cross
            std::uint64_t npx = mid[si] + (rng() % 7) - 3;
            ex.ModifyOrder(live[k], 1 + rng() % 60, npx);
        }
        // Replace/modify can remove an order from the book (fully filled or
        // IOC-like); prune ids we no longer find in the client book later.
        if (i % 64 == 0) drain();
        msgs = 0;
    }
    drain();
    (void)msgs;
    assert(unpaired() == 0 && "every Execute must pair with a TradeFill");

    for (SymbolId sym : syms) {
        const OrderBook &eb = ex.GetBook(sym);
        auto &cs = client.levels[static_cast<std::uint32_t>(sym)];
        compare_side(eb.bids(), cs[wire(Side::Buy)], sym, "bids");
        compare_side(eb.asks(), cs[wire(Side::Sell)], sym, "asks");
    }
    // Every live client order must exist in the engine, and counts match.
    std::size_t engine_live = 0;
    for (SymbolId sym : syms) engine_live += ex.GetBook(sym).bids().len() ? 0 : 0; // levels only; count below
    std::size_t engine_orders = 0;
    for (SymbolId sym : syms) {
        for (auto *n = ex.GetBook(sym).bids().GetHead(); n; n = n->forward[0]) engine_orders += n->value.GetSize();
        for (auto *n = ex.GetBook(sym).asks().GetHead(); n; n = n->forward[0]) engine_orders += n->value.GetSize();
    }
    (void)engine_live;
    assert(engine_orders == client.orders.size());
    // Submits report their own fills; replaces that execute do not (status
    // only), so the wire volume must be at least the submit tally. Match ids
    // are 1..N contiguous, so N == fills proves no fill was lost or doubled.
    assert(client.volume >= engine_volume && "wire volume < engine executed volume");
    // Match ids are shard-tagged: within each shard the low bits run 1..N
    // with N == fills seen for that shard, so no fill was lost or doubled.
    for (auto &[shard, ps] : client.per_shard)
        assert(ps.first == ps.second && "match ids not contiguous within shard: a fill was lost");

    std::printf("feed_reconstruct_test: OK  ops=%d shards=%d live_orders=%zu fills=%llu volume=%llu\n", ops, shards,
                client.orders.size(), (unsigned long long)client.fills,
                (unsigned long long)client.volume);
    return 0;
}
