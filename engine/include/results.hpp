#pragma once
#include "common.hpp"

enum class StatusCode {
    Success = 0,
    SymbolNotFound = 1,
    OrderNotFound = 2,
    Failed = 3,
    NotEnoughLiquidity = 4,
    FOKFailed = 5,
    InvalidPrice = 6,
    InvalidQuantity = 7,
    DuplicateOrder = 8,
};

enum class FillStatus {
    Accepted,        // rested on the book, nothing executed
    PartiallyFilled, // some executed; remainder rested or was dropped (IOC)
    FullyFilled,     // everything executed
    Rejected,        // nothing executed and nothing rested
};

struct FillResult {
    OrderId order_id;
    FillStatus fill_status;
    StatusCode status_code;
    Quantity qty_executed;
    Quantity qty_remaining; // unexecuted qty (resting on the book if `resting`)
    bool resting;           // true if qty_remaining now rests on the book

    FillResult(OrderId id);
};
