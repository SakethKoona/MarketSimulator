#pragma once
// Lightweight, centralized error types and helpers
// This file intentionally keeps existing enums (`OrderResult`, `ModifyResult`)
// and extends them with a repo-wide `ErrorCode` + `Error` + `Result<T>`.

#include <optional>
#include <string>
#include <variant>
#include <sstream>

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

// // Centralized error codes for the project. Keep values compact and
// // descriptive — add new codes here as the project grows.
// enum class ErrorCode {
//     Success = 0,
//     GenericFailure,
//     InvalidArgument,
//     SymbolNotFound,
//     OrderNotFound,
//     DuplicateOrder,
//     NotEnoughLiquidity,
//     IOError,
//     InternalError,
// };

// struct Error {
//     ErrorCode code{ErrorCode::GenericFailure};
//     std::string message;
//     std::optional<std::string> source; // optional context (function, subsystem)

//     Error() = default;
//     Error(ErrorCode c, std::string msg, std::optional<std::string> src = std::nullopt)
//         : code(c), message(std::move(msg)), source(std::move(src)) {}

//     std::string toString() const {
//         std::ostringstream ss;
//         ss << "Error(" << static_cast<int>(code) << "): " << message;
//         if (source) ss << " [" << *source << "]";
//         return ss.str();
//     }
// };

// // Result<T> is a small helper to return either a value or an Error.
// // Usage: Result<MyType> fn(...);
// template <typename T> using Result<T> = std::variant<T, Error>;

// // Specialization for void-like results: use ResultVoid
// using ResultVoid = std::variant<std::monostate, Error>;

// // Convenience helpers
// inline Error make_error(ErrorCode c, const std::string &msg, const std::optional<std::string> &src = std::nullopt) {
//     return Error{c, msg, src};
// }

// // Mapping helpers from existing enums to ErrorCode so we can incrementally
// // migrate code without touching every callsite at once.
// inline ErrorCode FromOrderResult(OrderResult r) {
//     switch (r) {
//     case OrderResult::Success:
//         return ErrorCode::Success;
//     case OrderResult::InvalidQty:
//         return ErrorCode::InvalidArgument;
//     case OrderResult::DuplicateOrder:
//         return ErrorCode::DuplicateOrder;
//     case OrderResult::OrderNotFound:
//         return ErrorCode::OrderNotFound;
//     case OrderResult::PriceOutOfRange:
//         return ErrorCode::InvalidArgument;
//     case OrderResult::TypeNotSupported:
//         return ErrorCode::InvalidArgument;
//     }
//     return ErrorCode::GenericFailure;
// }

// inline ErrorCode FromModifyResult(ModifyResult r) {
//     switch (r) {
//     case ModifyResult::Success:
//         return ErrorCode::Success;
//     case ModifyResult::Replaced:
//         return ErrorCode::Success;
//     case ModifyResult::OrderNotFound:
//         return ErrorCode::OrderNotFound;
//     case ModifyResult::Rejected:
//         return ErrorCode::GenericFailure;
//     case ModifyResult::QtyIncreaseNotAllowed:
//         return ErrorCode::InvalidArgument;
//     }
//     return ErrorCode::GenericFailure;
// }


// enum class ErrorCode {
//     Success,
//     NotFound,
//     InvalidArgument,
//     NotEnoughLiquidity,
//     Duplicate,
//     IOFailure,
//     Internal,
// };

// struct Error {
//     ErrorCode code;
//     std::string message;
//     std::optional<std::string> source;
// };

// template<typename T> using Result = std::variant<T, Error>;