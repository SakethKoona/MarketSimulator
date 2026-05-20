#pragma once
#include "common.hpp"

enum class StatusCode {
    Success = 0,
    SymbolNotFound = 1,
    OrderNotFound = 2,
    Failed = 3,
    NotEnoughLiquidity = 4,
    FOKFailed = 5,
};

enum class FillStatus {
    FullyFilled,
    PartiallyFilled,
    Rejected,
};

struct FillResult {
    OrderId order_id;
    FillStatus fill_status;
    StatusCode status_code;
    Quantity qty_executed;
    Quantity qty_remaining;

    FillResult(OrderId id);
};
