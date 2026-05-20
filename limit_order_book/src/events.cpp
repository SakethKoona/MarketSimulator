#include "events.hpp"
#include "common.hpp"
#include <cstdint>

int EventSink::emit_cancel_event(SymbolId symbol_id, OrderId order_id,
                                 uint64_t book_seq, Side side, Price price) {

    Timestamp ts = get_current_timestamp();

    OutBoundEvent event = OrderBookEvent{
        .symbol_id = symbol_id,
        .order_id = order_id,
        .action = BookAction::Delete,
        .side = side,
        .price = price,
        .qty = 0,
        .book_seq = book_seq,
        .ts_ns = ts,
    };

    this->emit(event);
    return 0;
}

int EventSink::emit_modify_event(SymbolId symbol_id, OrderId order_id,
                                 uint64_t book_seq, Side side, Price price,
                                 Quantity new_qty) {

    Timestamp ts = get_current_timestamp();

    OutBoundEvent event = OrderBookEvent{.symbol_id = symbol_id,
                                         .order_id = order_id,
                                         .action = BookAction::Modify,
                                         .side = side,
                                         .price = price,
                                         .qty = new_qty,
                                         .book_seq = book_seq,
                                         .ts_ns = ts};

    this->emit(event);
    return 0;
}

int EventSink::emit_add_event(SymbolId symbol_id, OrderId order_id,
                              uint64_t book_seq, Side side, Price price,
                              Quantity qty) {

    Timestamp ts = get_current_timestamp();

    OutBoundEvent event = OrderBookEvent{.symbol_id = symbol_id,
                                         .order_id = order_id,
                                         .action = BookAction::Add,
                                         .side = side,
                                         .price = price,
                                         .qty = qty,
                                         .book_seq = book_seq,
                                         .ts_ns = ts};

    this->emit(event);
    return 0;
}
