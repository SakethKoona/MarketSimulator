#pragma once

#include "common.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iosfwd>
#include <variant>
#include "spsc_ring.hpp"

// What happened to a resting order in the book. Mirrors the ITCH-style
// message set described in event_architecture.md.
enum class BookAction : uint8_t {
    Add,     // order accepted onto the book
    Modify,  // resting qty reduced by the owner (not by a trade)
    Execute, // resting qty reduced by a match; qty 0 means it left the book
    Replace, // cancel/replace: same id, new price and/or qty
    Delete,  // order removed from the book
};

struct OrderBookEvent {
    SymbolId symbol_id;
    OrderId order_id;
    BookAction action;
    Side side;    // Side
    Price price;  // Price level
    Quantity qty; // New resting Quantity (0 for Delete)
    uint64_t book_seq;
    Timestamp ts_ns;
};

// One match between an aggressor and a resting order. Paired with an
// Execute (or Delete) OrderBookEvent for the resting side carrying the same
// book_seq.
struct TradeFillEvent {
    SymbolId symbol_id;
    TradeId trade_id;

    OrderId aggressor_id;
    OrderId resting_id;

    Side aggressor_side;

    Price price;
    Quantity qty;
    uint64_t book_seq; // Should match a book_seq from an OrderBookEvent
    Timestamp ts_ns;
};

using OutBoundEvent = std::variant<OrderBookEvent, TradeFillEvent>;

const char *to_string(BookAction a);
std::ostream &operator<<(std::ostream &os, const OrderBookEvent &e);
std::ostream &operator<<(std::ostream &os, const TradeFillEvent &e);
std::ostream &operator<<(std::ostream &os, const OutBoundEvent &e);

class EventSink {
  public:
    // Capacity is rounded up to a power of two. The engine thread is the
    // only producer; exactly one consumer (the publisher, or the test
    // harness via Exchange::NextEvent) may call consume/peek/pop.
    explicit EventSink(std::size_t buffer_size)
        : buffer_(round_up_pow2(buffer_size)),
          fills_(round_up_pow2(buffer_size)) {}

    // Producer. Never blocks: a full ring drops the event and counts it in
    // dropped(); the publisher turns that into a feed gap.
    void emit(const OutBoundEvent &e) noexcept {
        buffer_.try_push(e);
        eventCounter_.fetch_add(1, std::memory_order_relaxed);
    }

    // Consumer, copying form. Pops the next event into an internal scratch
    // slot and returns a pointer to it, or nullptr when empty. The pointer
    // is valid until the next consume().
    OutBoundEvent *consume() {
        return buffer_.try_pop(last_) ? &last_ : nullptr;
    }

    // Consumer, zero-copy form for the publisher thread: peek() the front
    // event in place, then pop() it once encoded.
    OutBoundEvent *peek() noexcept { return buffer_.peek(); }
    void pop() noexcept { buffer_.pop(); }

    // Private copy of every TradeFillEvent, for the order-entry gateway to
    // build execution reports without touching the public feed ring. Same
    // SPSC rules: the engine thread produces, one consumer drains.
    SpscRing<TradeFillEvent> &fills() noexcept { return fills_; }

    // Events the producer could not enqueue because the ring was full.
    std::uint64_t dropped() const noexcept { return buffer_.dropped(); }
    std::size_t capacity() const noexcept { return buffer_.capacity(); }

    // One book event per mutation of a resting order
    void emit_book_event(BookAction action, SymbolId symbol_id,
                         OrderId order_id, uint64_t book_seq, Side side,
                         Price price, Quantity qty);

    // Convenience wrappers
    void emit_add_event(SymbolId symbol_id, OrderId order_id,
                        uint64_t book_seq, Side side, Price price,
                        Quantity qty);
    void emit_modify_event(SymbolId symbol_id, OrderId order_id,
                           uint64_t book_seq, Side side, Price price,
                           Quantity new_qty);
    void emit_execute_event(SymbolId symbol_id, OrderId order_id,
                            uint64_t book_seq, Side side, Price price,
                            Quantity remaining_qty);
    void emit_replace_event(SymbolId symbol_id, OrderId order_id,
                            uint64_t book_seq, Side side, Price price,
                            Quantity qty);
    void emit_cancel_event(SymbolId symbol_id, OrderId order_id,
                           uint64_t book_seq, Side side, Price price);

    void emit_trade_event(SymbolId symbol_id, TradeId trade_id,
                          OrderId aggressor_id, OrderId resting_id,
                          Side aggressor_side, Price price, Quantity qty,
                          uint64_t book_seq);

    // Total events ever emitted (not the number currently buffered)
    std::uint64_t emitted() const {
        return eventCounter_.load(std::memory_order_relaxed);
    }

  private:
    static std::size_t round_up_pow2(std::size_t n) {
        std::size_t p = 1;
        while (p < n)
            p <<= 1;
        return p;
    }

    std::atomic<std::uint64_t> eventCounter_{0};
    SpscRing<OutBoundEvent> buffer_;
    SpscRing<TradeFillEvent> fills_;
    OutBoundEvent last_{};
};
