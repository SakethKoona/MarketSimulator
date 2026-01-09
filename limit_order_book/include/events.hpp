
#pragma once

#include <cstddef>
#include <cstdlib>
#include <iostream>
struct Event {};

struct IEventSink {
    virtual void emit(const Event &e) noexcept = 0;
    virtual ~IEventSink() = default;
};

template <typename T> class RingBuffer {
  public:
    RingBuffer(std::size_t buffer_size)
        : size_(buffer_size), writeOffset_(0), readOffset_(0) {

        // Initialize the buffer, and allocate memory
        buffer = (T *)malloc(size_ * sizeof(T));
    }
    ~RingBuffer() { free(buffer); }

    T *push(const T &msg) {
        *(buffer + writeOffset_) = msg;
        T *ptr = (buffer + writeOffset_);
        writeOffset_++;
        return &msg;
    }

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
