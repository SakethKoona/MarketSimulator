#pragma once

#include "common.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <variant>

enum class BookAction : uint8_t {
    Add,
    Modify,
    Delete,
};

struct OrderBookEvent {
    SymbolId symbol_id;
    OrderId order_id;
    BookAction action;
    Side side;    // Side
    Price price;  // Price level
    Quantity qty; // New resting Quantity
    uint64_t book_seq;
    Timestamp ts_ns;
};

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

    void emit(const OutBoundEvent &e) noexcept { buffer_.push(e); }
    OutBoundEvent *consume() { return buffer_.pop(); }

    int emit_cancel_event(SymbolId symbol_id, OrderId order_id,
                          uint64_t book_seq, Side side, Price price) const;

    int emit_modify_event(SymbolId symbol_id, OrderId order_id,
                          uint64_t book_seq, Side side, Price price,
                          Quantity new_qty);

    int emit_add_event(SymbolId symbol_id, OrderId order_id, uint64_t book_seq,
                       Side side, Price price, Quantity qty);

    std::uint64_t nextEventId() {
        return eventCounter_.fetch_add(1, std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint64_t> eventCounter_;
    RingBuffer<OutBoundEvent> buffer_;
};
