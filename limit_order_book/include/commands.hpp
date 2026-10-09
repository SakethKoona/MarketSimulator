#pragma once
// Messages between the gateway thread and the engine thread. Both rings are
// SPSC: the gateway produces commands and consumes reports; the engine
// thread does the reverse. Fixed size, trivially copyable, no strings:
// cl_ord_id and session mapping stay in the gateway.
#include "common.hpp"
#include "orderbook.hpp"
#include "results.hpp"
#include <cstdint>

enum class CommandType : std::uint8_t { NewOrder = 1, Cancel = 2, Modify = 3 };

struct InboundCommand {
    std::uint64_t request_id; // gateway-assigned; echoed on the report
    CommandType type;
    Side side;
    OrderType ord_type;
    TypeInForce tif;
    std::uint32_t symbol_id;
    std::uint32_t qty;
    std::uint64_t price;    // Modify: 0 keeps the current price
    std::uint64_t order_id; // Cancel/Modify target
    std::uint64_t ts_ns;    // gateway receive time (monotonic)
};
static_assert(sizeof(InboundCommand) <= 64);

enum class ReportKind : std::uint8_t {
    Accepted = 1,  // new order accepted (may have filled; leaves may be 0)
    Rejected = 2,  // new order / cancel / modify rejected
    Cancelled = 3, // cancel done, or IOC/FOK remainder dropped
    Modified = 4,  // modify done (shrink or replace)
    Execution = 5, // one fill on order_id; request_id is 0 for resting side
};

struct OrderReport {
    std::uint64_t request_id; // 0 when unsolicited (resting-side fill)
    std::uint64_t order_id;
    ReportKind kind;
    StatusCode status; // reason on Rejected
    Side side;         // Execution / Accepted
    std::uint32_t symbol_id;
    std::uint32_t qty;        // Accepted: order qty. Modified: new qty
    std::uint32_t last_qty;   // Execution: filled qty
    std::uint32_t leaves_qty; // Accepted/Modified: resting qty after the op
    std::uint64_t price;      // Accepted/Modified: order px. Execution: fill px
    std::uint64_t match_id;   // Execution
    std::uint64_t ts_ns;      // wall clock
};
static_assert(sizeof(OrderReport) <= 80);
