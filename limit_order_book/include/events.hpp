
#pragma once

#include <cstddef>
struct Event {};

struct IEventSink {
    virtual void emit(const Event &e) noexcept = 0;
    virtual ~IEventSink() = default;
};

template <typename T> class RingBuffer {
  public:
    RingBuffer(std::size_t buffer_size) : size_(buffer_size) {}
    ~RingBuffer() { delete[] buffer; }

    bool push(const T &msg) {}
    bool read() {}
    bool peek() {}

    std::size_t len() {}

  private:
    T *buffer;
    std::size_t writeOffset_;
    std::size_t readOffset_;
    std::size_t size_;
};

class EventSink : IEventSink {
  public:
    void emit(const Event &e) noexcept {

        int x = 5;
        int y = 2;
    }

  private:
    RingBuffer<Event> buffer_;
};
