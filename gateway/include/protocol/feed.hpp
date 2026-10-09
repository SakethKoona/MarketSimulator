#pragma once
// Wire structs for the public market data feed. Contract: docs/protocol/feed-v1.md
// Little-endian, packed. Every struct's size is asserted against the spec.
#include <cstddef>
#include <cstdint>

namespace feed {

constexpr std::uint16_t kVersion = 1;
constexpr std::uint8_t kSideBuy = 'B';
constexpr std::uint8_t kSideSell = 'S';

enum class MsgType : std::uint8_t {
    SystemEvent = 'S',
    StockDirectory = 'R',
    AddOrder = 'A',
    OrderExecuted = 'E',
    OrderCancel = 'X',
    OrderDelete = 'D',
    OrderReplace = 'U',
};

enum class SystemEventCode : std::uint8_t {
    StartOfSession = 'O',
    EndOfSession = 'C',
};

#pragma pack(push, 1)

struct SystemEvent {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::SystemEvent);
    std::uint8_t event_code;
    std::uint16_t version = kVersion;
    std::uint64_t ts_ns;
};
static_assert(sizeof(SystemEvent) == 12);

struct StockDirectory {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::StockDirectory);
    std::uint8_t price_scale = 0;
    std::uint16_t reserved = 0;
    std::uint32_t symbol_id;
    char ticker[8];
    std::uint64_t ts_ns;
};
static_assert(sizeof(StockDirectory) == 24);

struct AddOrder {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::AddOrder);
    std::uint8_t side;
    std::uint32_t symbol_id;
    std::uint64_t order_id;
    std::uint64_t price;
    std::uint32_t qty;
    std::uint64_t book_seq;
    std::uint64_t ts_ns;
};
static_assert(sizeof(AddOrder) == 42);

struct OrderExecuted {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::OrderExecuted);
    std::uint8_t side; // side of the resting order
    std::uint32_t symbol_id;
    std::uint64_t order_id; // resting order
    std::uint64_t price;
    std::uint32_t exec_qty;
    std::uint32_t remaining_qty;
    std::uint64_t match_id;
    std::uint64_t book_seq;
    std::uint64_t ts_ns;
};
static_assert(sizeof(OrderExecuted) == 54);

struct OrderCancel {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::OrderCancel);
    std::uint32_t symbol_id;
    std::uint64_t order_id;
    std::uint32_t remaining_qty;
    std::uint64_t book_seq;
    std::uint64_t ts_ns;
};
static_assert(sizeof(OrderCancel) == 33);

struct OrderDelete {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::OrderDelete);
    std::uint32_t symbol_id;
    std::uint64_t order_id;
    std::uint64_t book_seq;
    std::uint64_t ts_ns;
};
static_assert(sizeof(OrderDelete) == 29);

struct OrderReplace {
    std::uint8_t type = static_cast<std::uint8_t>(MsgType::OrderReplace);
    std::uint8_t side;
    std::uint32_t symbol_id;
    std::uint64_t order_id;
    std::uint64_t price;
    std::uint32_t qty;
    std::uint64_t book_seq;
    std::uint64_t ts_ns;
};
static_assert(sizeof(OrderReplace) == 42);

#pragma pack(pop)

// Largest feed message; used to size encode buffers.
constexpr std::size_t kMaxMessageSize = sizeof(OrderExecuted);

} // namespace feed
