#pragma once

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <types.hpp>

enum class EventType {
    OrderAdded,
    OrderModified,
    OrderCanceled,
    TradeExecuted,
    OrderSubmitted,
};

struct AddOrderEvent {
    std::string symbol;
    OrderRefNumber ref_number;
    Price price;
    Quantity qty;
    Side side;
};

struct DeleteOrderEvent {
    std::string symbol;
    OrderRefNumber ref_number;
};

struct PartialCancelOrderEvent {
    std::string symbol;
    OrderRefNumber ref_number;
    Quantity newQty;
};

struct OrderExecutedEvent {
    OrderRefNumber ref_number;
    std::string symbol;
    Quantity executed_qty;
    MatchNumber match_num;
};

struct OrderRepalceEvent {
    OrderRefNumber old_ref_number;
    std::string symbol;
    OrderRefNumber new_ref_number;
    Quantity qty;
    Price price;
};

struct TradeEvent {
    OrderRefNumber ref_number;
    std::string symbol;
    Price price;
    Quantity qty;
    Side side;
    MatchNumber match_num;
};

struct Event {
    EventType type;
    Timestamp timeGenerated;

    union {
        AddOrderEvent add_event;
        PartialCancelOrderEvent partial_cancel;
        DeleteOrderEvent delete_event;
        OrderExecutedEvent order_executed;
        OrderRepalceEvent order_replace;
        TradeEvent trade;
    };
};

struct IEventSink {
    virtual void emit(const Event &e) noexcept = 0;
    virtual ~IEventSink() = default;
};

// TODO: Make this lock free
template <typename T> class RingBuffer {
  public:
    RingBuffer(std::size_t buffer_size)
        : size_(buffer_size), writeOffset_(0), readOffset_(0) {

        // Initialize the buffer, and allocate memory
        buffer = (T *)malloc(size_ * sizeof(T));
    }
    // ~RingBuffer() { free(buffer); }

    // In current design, messages can be
    // overwritten, which is fast, in the future, we might need
    // to make that more safe
    T *push(const T &msg) {
        *(buffer + writeOffset_) = msg;
        T *ptr = (buffer + writeOffset_);
        writeOffset_ = (writeOffset_ + 1) % size_;
        return ptr;
    }

    T *pop() {
        if (writeOffset_ == readOffset_) // Empty buffer
            return nullptr;
        T *msgPtr = (buffer + readOffset_);
        readOffset_ = (readOffset_ + 1) % size_;
        return msgPtr;
    }

    bool peek() {}

  private:
    T *buffer;
    std::size_t size_;
    std::size_t writeOffset_;
    std::size_t readOffset_;
};

class EventSink : IEventSink {
  public:
    EventSink(std::size_t buffer_size) : buffer_(buffer_size) {}

    void emit(const Event &e) noexcept { buffer_.push(e); }
    Event *consume() { return buffer_.pop(); }

  private:
    RingBuffer<Event> buffer_;
};
