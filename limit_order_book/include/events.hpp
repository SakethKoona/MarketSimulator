
#pragma once

#include <cstddef>
struct Event {};

struct IEventSink {
    virtual void emit(const Event &e) noexcept = 0;
    virtual ~IEventSink() = default;
};

template <typename T> class RingBuffer {
  public:
    RingBuffer<T>(std::size_t buffer_size) : size_(buffer_size) {}

  private:
    T *writePtr_;
    T *readPtr_;
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
