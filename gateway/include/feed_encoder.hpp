#pragma once
// Translates engine events (events.hpp) into feed wire messages (feed.hpp).
#include "events.hpp"
#include "protocol/feed.hpp"
#include <cstring>

namespace feed {

inline std::uint8_t wire_side(Side s) { return s == Side::Buy ? kSideBuy : kSideSell; }

inline std::uint16_t encode_add(const OrderBookEvent &e, char *out) {
    AddOrder m{};
    m.side = wire_side(e.side);
    m.symbol_id = static_cast<std::uint32_t>(e.symbol_id);
    m.order_id = e.order_id;
    m.price = e.price;
    m.qty = static_cast<std::uint32_t>(e.qty);
    m.book_seq = e.book_seq;
    m.ts_ns = e.ts_ns;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

inline std::uint16_t encode_replace(const OrderBookEvent &e, char *out) {
    OrderReplace m{};
    m.side = wire_side(e.side);
    m.symbol_id = static_cast<std::uint32_t>(e.symbol_id);
    m.order_id = e.order_id;
    m.price = e.price;
    m.qty = static_cast<std::uint32_t>(e.qty);
    m.book_seq = e.book_seq;
    m.ts_ns = e.ts_ns;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

inline std::uint16_t encode_cancel(const OrderBookEvent &e, char *out) {
    OrderCancel m{};
    m.symbol_id = static_cast<std::uint32_t>(e.symbol_id);
    m.order_id = e.order_id;
    m.remaining_qty = static_cast<std::uint32_t>(e.qty);
    m.book_seq = e.book_seq;
    m.ts_ns = e.ts_ns;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

inline std::uint16_t encode_delete(const OrderBookEvent &e, char *out) {
    OrderDelete m{};
    m.symbol_id = static_cast<std::uint32_t>(e.symbol_id);
    m.order_id = e.order_id;
    m.book_seq = e.book_seq;
    m.ts_ns = e.ts_ns;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

// One Order Executed from the engine's (Execute, TradeFill) pair. The pair
// shares book_seq; the Execute carries the resting side and remaining qty,
// the TradeFill carries executed qty and the match id.
inline std::uint16_t encode_executed(const OrderBookEvent &exec,
                                     const TradeFillEvent &fill, char *out) {
    OrderExecuted m{};
    m.side = wire_side(exec.side);
    m.symbol_id = static_cast<std::uint32_t>(exec.symbol_id);
    m.order_id = exec.order_id;
    m.price = fill.price;
    m.exec_qty = static_cast<std::uint32_t>(fill.qty);
    m.remaining_qty = static_cast<std::uint32_t>(exec.qty);
    m.match_id = fill.trade_id;
    m.book_seq = exec.book_seq;
    m.ts_ns = exec.ts_ns;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

inline std::uint16_t encode_system_event(SystemEventCode code, std::uint64_t ts,
                                         char *out) {
    SystemEvent m{};
    m.event_code = static_cast<std::uint8_t>(code);
    m.ts_ns = ts;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

inline std::uint16_t encode_directory(std::uint32_t symbol_id,
                                      const std::string &ticker,
                                      std::uint64_t ts, char *out) {
    StockDirectory m{};
    m.symbol_id = symbol_id;
    std::memset(m.ticker, ' ', sizeof(m.ticker));
    std::memcpy(m.ticker, ticker.data(),
                ticker.size() < sizeof(m.ticker) ? ticker.size() : sizeof(m.ticker));
    m.ts_ns = ts;
    std::memcpy(out, &m, sizeof(m));
    return sizeof(m);
}

} // namespace feed
