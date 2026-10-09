#pragma once
// Engine-thread side of the gateway: applies InboundCommands to the
// Exchange and answers with OrderReports, including one Execution per fill
// for both sides, taken from EventSink::fills().
#include "commands.hpp"
#include "exchange.hpp"
#include "spsc_ring.hpp"
#include <optional>

class EngineLoop {
  public:
    // One EngineLoop per shard, driven by that shard's thread only. It
    // must only receive commands for symbols/orders living on `shard`.
    EngineLoop(Exchange &ex, std::size_t shard, SpscRing<InboundCommand> &commands,
               SpscRing<OrderReport> &reports)
        : ex_(ex), shard_(shard), commands_(commands), reports_(reports) {}

    // Applies up to max_commands and drains pending fills. Returns the
    // number of commands applied.
    std::size_t pump(std::size_t max_commands = 256) {
        std::size_t n = 0;
        InboundCommand c;
        while (n < max_commands && commands_.try_pop(c)) {
            handle(c);
            ++n;
        }
        drain_fills();
        return n;
    }

    std::uint64_t report_drops() const { return report_drops_; }

  private:
    void push(OrderReport r) {
        r.ts_ns = get_current_timestamp();
        // Reports must not be lost; spin briefly if the gateway is behind.
        for (int i = 0; i < 1000; ++i)
            if (reports_.try_push(r))
                return;
        ++report_drops_;
    }

    void drain_fills() {
        TradeFillEvent f;
        while (ex_.Sink(shard_).fills().try_pop(f)) {
            OrderReport a{};
            a.kind = ReportKind::Execution;
            a.symbol_id = static_cast<std::uint32_t>(f.symbol_id);
            a.price = f.price;
            a.last_qty = static_cast<std::uint32_t>(f.qty);
            a.match_id = f.trade_id;
            a.status = StatusCode::Success;

            a.order_id = f.resting_id;
            a.side = f.aggressor_side == Side::Buy ? Side::Sell : Side::Buy;
            push(a);

            a.order_id = f.aggressor_id;
            a.side = f.aggressor_side;
            push(a);
        }
    }

    void handle(const InboundCommand &c) {
        switch (c.type) {
        case CommandType::NewOrder: {
            FillResult res = ex_.SubmitOrder(c.symbol_id, c.price, c.qty, c.side,
                                             c.ord_type, c.tif);
            OrderReport r{};
            r.request_id = c.request_id;
            r.order_id = res.order_id;
            r.side = c.side;
            r.symbol_id = c.symbol_id;
            r.qty = c.qty;
            r.price = c.price;
            r.status = res.status_code;
            if (res.fill_status == FillStatus::Rejected) {
                r.kind = ReportKind::Rejected;
                push(r);
                return;
            }
            r.kind = ReportKind::Accepted;
            r.leaves_qty = res.resting ? static_cast<std::uint32_t>(res.qty_remaining) : 0;
            push(r);
            drain_fills(); // executions follow the ack
            if (!res.resting && res.qty_remaining > 0) {
                OrderReport k{};
                k.request_id = 0; // pending entry already consumed by the ack
                k.order_id = res.order_id;
                k.kind = ReportKind::Cancelled;
                k.status = c.tif == TypeInForce::FOK ? StatusCode::FOKFailed : StatusCode::Success;
                push(k);
            }
            return;
        }
        case CommandType::Cancel: {
            StatusCode sc = ex_.CancelOrder(c.order_id);
            OrderReport r{};
            r.request_id = c.request_id;
            r.order_id = c.order_id;
            r.status = sc;
            r.kind = sc == StatusCode::Success ? ReportKind::Cancelled : ReportKind::Rejected;
            push(r);
            return;
        }
        case CommandType::Modify: {
            std::optional<Price> px;
            if (c.price)
                px = c.price;
            StatusCode sc = ex_.ModifyOrder(c.order_id, c.qty, px);
            OrderReport r{};
            r.request_id = c.request_id;
            r.order_id = c.order_id;
            r.status = sc;
            r.qty = c.qty;
            r.price = c.price;
            r.kind = sc == StatusCode::Success ? ReportKind::Modified : ReportKind::Rejected;
            push(r);
            drain_fills(); // a crossing replace executes right away
            return;
        }
        }
    }

    Exchange &ex_;
    std::size_t shard_;
    SpscRing<InboundCommand> &commands_;
    SpscRing<OrderReport> &reports_;
    std::uint64_t report_drops_ = 0;
};
