#pragma once
// Wire structs for Binary Order Entry. Contract: docs/protocol/boe-v1.md
#include <cstddef>
#include <cstdint>

namespace boe {

constexpr std::uint16_t kStartOfMessage = 0xBABA;
constexpr std::size_t kClOrdIdLen = 20;
constexpr std::size_t kSymbolLen = 8;
constexpr std::size_t kTextLen = 60;

enum class MsgType : std::uint8_t {
    // client → server
    LoginRequest = 0x01,
    LogoutRequest = 0x02,
    ClientHeartbeat = 0x03,
    NewOrder = 0x04,
    CancelOrder = 0x05,
    ModifyOrder = 0x06,
    // server → client
    LoginResponse = 0x07,
    Logout = 0x08,
    ServerHeartbeat = 0x09,
    OrderAcknowledgment = 0x25,
    OrderRejected = 0x26,
    OrderModified = 0x27,
    OrderCancelled = 0x28,
    OrderExecution = 0x2C,
};

namespace side { constexpr std::uint8_t Buy = 'B', Sell = 'S'; }
namespace ord_type { constexpr std::uint8_t Market = '1', Limit = '2'; }
namespace tif { constexpr std::uint8_t GTC = '0', IOC = '3', FOK = '4'; }
namespace login_status {
constexpr std::uint8_t Accepted = 'A', NotAuthorised = 'N', Duplicate = 'D',
                       BadVersion = 'B';
}
namespace reject_reason {
constexpr std::uint8_t UnknownSymbol = 'S', BadQty = 'Q', BadPrice = 'P',
                       FokUnfillable = 'K', DuplicateClOrdId = 'D',
                       UnknownOrder = 'U', NotLoggedIn = 'X', Other = 'O';
}
namespace cancel_reason {
constexpr std::uint8_t User = 'U', IocRemainder = 'I', FokFailed = 'K',
                       Replaced = 'R';
}

#pragma pack(push, 1)

struct Header {
    std::uint16_t start_of_message = kStartOfMessage;
    std::uint16_t message_length; // bytes after this field
    std::uint8_t message_type;
    std::uint8_t matching_unit = 0;
    std::uint32_t sequence_number;
};
static_assert(sizeof(Header) == 10);
// Header bytes counted by message_length (type + unit + seq).
constexpr std::uint16_t kHeaderTail = 6;

// ---- session, client → server ----
struct LoginRequest {
    char session_sub_id[4];
    char username[4];
    char password[10];
};
static_assert(sizeof(LoginRequest) == 18);

// ---- session, server → client ----
struct LoginResponse {
    std::uint8_t status;
    std::uint32_t last_received_seq;
    char text[kTextLen];
};
static_assert(sizeof(LoginResponse) == 65);

struct Logout {
    std::uint8_t reason;
    char text[kTextLen];
};
static_assert(sizeof(Logout) == 61);

// ---- orders, client → server ----
struct NewOrder {
    char cl_ord_id[kClOrdIdLen];
    std::uint8_t side;
    std::uint32_t qty;
    std::uint64_t price;
    char symbol[kSymbolLen];
    std::uint8_t ord_type;
    std::uint8_t tif;
};
static_assert(sizeof(NewOrder) == 43);

struct CancelOrder {
    char orig_cl_ord_id[kClOrdIdLen];
};
static_assert(sizeof(CancelOrder) == 20);

struct ModifyOrder {
    char cl_ord_id[kClOrdIdLen];
    char orig_cl_ord_id[kClOrdIdLen];
    std::uint32_t qty;
    std::uint64_t price; // 0 keeps the current price
};
static_assert(sizeof(ModifyOrder) == 52);

// ---- execution reports, server → client ----
struct OrderAcknowledgment {
    std::uint64_t ts_ns;
    char cl_ord_id[kClOrdIdLen];
    std::uint64_t order_id;
    char symbol[kSymbolLen];
    std::uint8_t side;
    std::uint32_t qty;
    std::uint64_t price;
    std::uint32_t leaves_qty;
};
static_assert(sizeof(OrderAcknowledgment) == 61);

struct OrderRejected {
    std::uint64_t ts_ns;
    char cl_ord_id[kClOrdIdLen];
    std::uint8_t reason;
    char text[kTextLen];
};
static_assert(sizeof(OrderRejected) == 89);

struct OrderModified {
    std::uint64_t ts_ns;
    char cl_ord_id[kClOrdIdLen];
    std::uint64_t order_id;
    std::uint32_t qty;
    std::uint64_t price;
    std::uint32_t leaves_qty;
};
static_assert(sizeof(OrderModified) == 52);

struct OrderCancelled {
    std::uint64_t ts_ns;
    char cl_ord_id[kClOrdIdLen];
    std::uint64_t order_id;
    std::uint8_t reason;
};
static_assert(sizeof(OrderCancelled) == 37);

struct OrderExecution {
    std::uint64_t ts_ns;
    char cl_ord_id[kClOrdIdLen];
    std::uint64_t order_id;
    std::uint64_t match_id;
    std::uint32_t last_qty;
    std::uint64_t last_price;
    std::uint32_t leaves_qty;
    std::uint8_t side;
};
static_assert(sizeof(OrderExecution) == 61);

#pragma pack(pop)

// Largest body; sizes a per-session read buffer.
constexpr std::size_t kMaxBodySize = sizeof(OrderRejected);
constexpr std::size_t kMaxFrameSize = sizeof(Header) + kMaxBodySize;

} // namespace boe
