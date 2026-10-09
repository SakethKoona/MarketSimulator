#pragma once

#include "common.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iosfwd>
#include <variant>

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

// TODO: Make this lock free
template <typename T> class RingBuffer {
  public:
    RingBuffer(std::size_t buffer_size)
        : size_(buffer_size), writeOffset_(0), readOffset_(0) {

        // Initialize the buffer, and allocate memory
        buffer = (T *)malloc(size_ * sizeof(T));
    }

    // In current design, messages can be
    // overwritten, which is fast, in the future, we might need
    // to make that more safe

    // Returns a pointer to the event
    T *push(const T &msg) {
        *(buffer + writeOffset_) = msg;
        T *ptr = (buffer + writeOffset_);
        writeOffset_ = (writeOffset_ + 1) % size_;
        return ptr;
    }

    // Once popped, we return a pointer to the popped object
    T *pop() {
        if (writeOffset_ == readOffset_) // Empty buffer
            return nullptr;
        T *msgPtr = (buffer + readOffset_);
        readOffset_ = (readOffset_ + 1) % size_;
        return msgPtr;
    }

    // Looks at value without popping and adjusting offsets
    T *peek() {
        if (writeOffset_ == readOffset_) {
            return nullptr;
        }
        return (buffer + readOffset_);
    }

  private:
    T *buffer;
    std::size_t size_;
    std::size_t writeOffset_;
    std::size_t readOffset_;
};

class EventSink {
  public:
    EventSink(std::size_t buffer_size) : buffer_(buffer_size) {}

    void emit(const OutBoundEvent &e) noexcept {
        buffer_.push(e);
        eventCounter_.fetch_add(1, std::memory_order_relaxed);
    }
    OutBoundEvent *consume() { return buffer_.pop(); }

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
    std::atomic<std::uint64_t> eventCounter_{0};
    RingBuffer<OutBoundEvent> buffer_;
};
