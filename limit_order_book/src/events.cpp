#include "events.hpp"
#include "common.hpp"
#include <cstdint>
#include <ostream>

const char *to_string(BookAction a) {
    switch (a) {
    case BookAction::Add:
        return "ADD";
    case BookAction::Modify:
        return "MODIFY";
    case BookAction::Execute:
        return "EXECUTE";
    case BookAction::Replace:
        return "REPLACE";
    case BookAction::Delete:
        return "DELETE";
    }
    return "UNKNOWN";
}

std::ostream &operator<<(std::ostream &os, const OrderBookEvent &e) {
    return os << "[seq " << e.book_seq << "] " << to_string(e.action)
              << " sym=" << e.symbol_id << " order=" << e.order_id
              << " side=" << (e.side == Side::Buy ? "BUY" : "SELL")
              << " px=" << e.price << " qty=" << e.qty;
}

std::ostream &operator<<(std::ostream &os, const TradeFillEvent &e) {
    return os << "[seq " << e.book_seq << "] TRADE sym=" << e.symbol_id
              << " trade=" << e.trade_id << " aggressor=" << e.aggressor_id
              << " resting=" << e.resting_id << " aggr_side="
              << (e.aggressor_side == Side::Buy ? "BUY" : "SELL")
              << " px=" << e.price << " qty=" << e.qty;
}

std::ostream &operator<<(std::ostream &os, const OutBoundEvent &e) {
    std::visit([&os](const auto &ev) { os << ev; }, e);
    return os;
}

void EventSink::emit_book_event(BookAction action, SymbolId symbol_id,
                                OrderId order_id, uint64_t book_seq, Side side,
                                Price price, Quantity qty) {
    OutBoundEvent event = OrderBookEvent{
        .symbol_id = symbol_id,
        .order_id = order_id,
        .action = action,
        .side = side,
        .price = price,
        .qty = qty,
        .book_seq = book_seq,
        .ts_ns = get_current_timestamp(),
    };
    this->emit(event);
}

void EventSink::emit_add_event(SymbolId symbol_id, OrderId order_id,
                               uint64_t book_seq, Side side, Price price,
                               Quantity qty) {
    emit_book_event(BookAction::Add, symbol_id, order_id, book_seq, side,
                    price, qty);
}

void EventSink::emit_modify_event(SymbolId symbol_id, OrderId order_id,
                                  uint64_t book_seq, Side side, Price price,
                                  Quantity new_qty) {
    emit_book_event(BookAction::Modify, symbol_id, order_id, book_seq, side,
                    price, new_qty);
}

void EventSink::emit_execute_event(SymbolId symbol_id, OrderId order_id,
                                   uint64_t book_seq, Side side, Price price,
                                   Quantity remaining_qty) {
    emit_book_event(BookAction::Execute, symbol_id, order_id, book_seq, side,
                    price, remaining_qty);
}

void EventSink::emit_replace_event(SymbolId symbol_id, OrderId order_id,
                                   uint64_t book_seq, Side side, Price price,
                                   Quantity qty) {
    emit_book_event(BookAction::Replace, symbol_id, order_id, book_seq, side,
                    price, qty);
}

void EventSink::emit_cancel_event(SymbolId symbol_id, OrderId order_id,
                                  uint64_t book_seq, Side side, Price price) {
    emit_book_event(BookAction::Delete, symbol_id, order_id, book_seq, side,
                    price, 0);
}

void EventSink::emit_trade_event(SymbolId symbol_id, TradeId trade_id,
                                 OrderId aggressor_id, OrderId resting_id,
                                 Side aggressor_side, Price price, Quantity qty,
                                 uint64_t book_seq) {
    OutBoundEvent event = TradeFillEvent{
        .symbol_id = symbol_id,
        .trade_id = trade_id,
        .aggressor_id = aggressor_id,
        .resting_id = resting_id,
        .aggressor_side = aggressor_side,
        .price = price,
        .qty = qty,
        .book_seq = book_seq,
        .ts_ns = get_current_timestamp(),
    };
    this->emit(event);
}
