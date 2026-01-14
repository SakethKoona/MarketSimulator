#pragma once

enum class OrderResult {
    Success,
    InvalidQty,
    DuplicateOrder,
    OrderNotFound,
    PriceOutOfRange,
    TypeNotSupported,
};

enum class ModifyResult {
    Success,
    Replaced,
    OrderNotFound,
    Rejected,
    QtyIncreaseNotAllowed
};
