// ingress_core_test: the private report stream an adapter sees, driven
// single-threaded through OrderEntryCore (submit -> pump -> poll). Checks
// the leaves_qty contract on every report kind, in particular that an
// aggressor sweeping several resting orders gets one EXECUTION per fill
// with leaves counting down to 0 (the bug the python population found).
#include "exchange.hpp"
#include "ingress/core.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

namespace {

struct Sess {
    OrderEntryCore &core;
    mktsim_session *s;
    std::vector<mktsim_report> got;
    explicit Sess(OrderEntryCore &c, const char *name) : core(c), s(c.open_session(name)) { assert(s); }
    ~Sess() { core.close_session(s); }
    void send(std::uint64_t req, std::uint8_t side, std::uint32_t qty, std::uint64_t px, std::uint8_t tif) {
        mktsim_order_req r{};
        r.request_id = req; r.kind = MKTSIM_REQ_NEW; r.side = side; r.ord_type = MKTSIM_LIMIT;
        r.tif = tif; r.symbol_id = 0; r.qty = qty; r.price = px;
        int rc = core.submit(s, r);
        assert(rc == MKTSIM_OK);
    }
    void modify(std::uint64_t req, std::uint64_t oid, std::uint32_t qty, std::uint64_t px) {
        mktsim_order_req r{};
        r.request_id = req; r.kind = MKTSIM_REQ_MODIFY; r.order_id = oid; r.qty = qty; r.price = px;
        int rc = core.submit(s, r);
        assert(rc == MKTSIM_OK);
    }
    std::vector<mktsim_report> drain() {
        got.clear();
        core.poll(s, [](void *u, const mktsim_report *r) { static_cast<Sess *>(u)->got.push_back(*r); }, this, 1024);
        return got;
    }
};

void expect_exec(const mktsim_report &r, std::uint64_t oid, std::uint32_t last, std::uint32_t leaves) {
    assert(r.kind == MKTSIM_RPT_EXECUTION);
    assert(r.order_id == oid);
    assert(r.last_qty == last);
    assert(r.leaves_qty == leaves);
    assert(r.request_id == 0);
}

} // namespace

int main() {
    json cfg = {{"symbols", {{"AAA", json::object()}}}, {"sink_size", 1 << 12}, {"seq_capacity", 10},
                {"num_symbols", 1}, {"shards", 1}};
    Exchange ex(cfg);
    OrderEntryCore core(ex, OrderEntryCore::Config{});
    assert(core.num_shards() == 1);
    auto pump = [&] { while (core.pump(0)) {} };

    // 1. Sweep: two resting sells, one IOC buy for both. Aggressor sees two
    //    executions with leaves 10 then 0 and no CANCELLED; each resting
    //    order sees one execution with leaves 0.
    {
        Sess a(core, "a"), b(core, "b");
        a.send(1, MKTSIM_SELL, 10, 20000, MKTSIM_GTC);
        a.send(2, MKTSIM_SELL, 10, 20001, MKTSIM_GTC);
        pump();
        auto ra = a.drain();
        assert(ra.size() == 2 && ra[0].kind == MKTSIM_RPT_ACCEPTED && ra[0].leaves_qty == 10 && ra[1].leaves_qty == 10);
        std::uint64_t o1 = ra[0].order_id, o2 = ra[1].order_id;

        b.send(3, MKTSIM_BUY, 20, 20001, MKTSIM_IOC);
        pump();
        auto rb = b.drain();
        assert(rb.size() == 3);
        assert(rb[0].kind == MKTSIM_RPT_ACCEPTED && rb[0].request_id == 3 && rb[0].leaves_qty == 0);
        std::uint64_t ob = rb[0].order_id;
        expect_exec(rb[1], ob, 10, 10);
        expect_exec(rb[2], ob, 10, 0);
        assert(rb[1].price == 20000 && rb[2].price == 20001);
        assert(rb[1].match_id != rb[2].match_id);
        ra = a.drain();
        assert(ra.size() == 2);
        expect_exec(ra[0], o1, 10, 0);
        expect_exec(ra[1], o2, 10, 0);
        assert(ra[0].match_id == rb[1].match_id && ra[1].match_id == rb[2].match_id);
    }

    // 2. Partial against a resting order: resting leaves 6, aggressor done.
    //    Then an IOC bigger than the book: leaves counts down and the
    //    remainder is CANCELLED after the fills.
    {
        Sess a(core, "a"), b(core, "b");
        a.send(1, MKTSIM_SELL, 10, 30000, MKTSIM_GTC);
        pump();
        std::uint64_t o1 = a.drain()[0].order_id;
        b.send(2, MKTSIM_BUY, 4, 30000, MKTSIM_IOC);
        pump();
        auto rb = b.drain();
        assert(rb.size() == 2 && rb[0].kind == MKTSIM_RPT_ACCEPTED);
        expect_exec(rb[1], rb[0].order_id, 4, 0);
        auto ra = a.drain();
        assert(ra.size() == 1);
        expect_exec(ra[0], o1, 4, 6);

        b.send(3, MKTSIM_BUY, 15, 30000, MKTSIM_IOC);
        pump();
        rb = b.drain();
        assert(rb.size() == 3 && rb[0].kind == MKTSIM_RPT_ACCEPTED);
        expect_exec(rb[1], rb[0].order_id, 6, 9);
        assert(rb[2].kind == MKTSIM_RPT_CANCELLED && rb[2].order_id == rb[0].order_id);
        ra = a.drain();
        assert(ra.size() == 1);
        expect_exec(ra[0], o1, 6, 0);
        // The resting order is finished: a later fill cannot be routed to it.
        assert(ex.GetBook(0).FindOrder(o1) == nullptr);
    }

    // 3. A resting GTC buy that sweeps two sells on a crossing modify
    //    (cancel-replace to a higher price) also counts leaves down.
    {
        Sess a(core, "a"), b(core, "b");
        b.send(1, MKTSIM_BUY, 25, 10000, MKTSIM_GTC);
        a.send(2, MKTSIM_SELL, 10, 10005, MKTSIM_GTC);
        a.send(3, MKTSIM_SELL, 10, 10006, MKTSIM_GTC);
        pump();
        std::uint64_t ob = b.drain()[0].order_id;
        auto ra = a.drain();
        assert(ra.size() == 2);
        b.modify(4, ob, 25, 10006);
        pump();
        auto rb = b.drain();
        assert(rb.size() == 3);
        assert(rb[0].kind == MKTSIM_RPT_MODIFIED && rb[0].request_id == 4);
        expect_exec(rb[1], ob, 10, 15);
        expect_exec(rb[2], ob, 10, 5);
        ra = a.drain();
        assert(ra.size() == 2 && ra[0].leaves_qty == 0 && ra[1].leaves_qty == 0);
        assert(ex.GetBook(0).FindOrder(ob)->quantity == 5);
    }

    std::printf("ingress_core_test: ok\n");
    return 0;
}
