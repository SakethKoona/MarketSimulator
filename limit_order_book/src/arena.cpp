#include "arena.hpp"

ArenaAllocator::ArenaAllocator(std::size_t size) : capacity_(size), offset_(0) {
    // We allocate all the memory at once
    buffer_ = static_cast<char *>(::operator new(size));
}

void *ArenaAllocator::allocate(std::size_t size, std::size_t alignment) {
    // So, in this function, we want to do the following:
    // 1. Determine if we even have enough space to allocate this, otherwise
    // throw or return a nullptr
    // 2. adjust the offset_
    // 3. return the memory that was allocated

    std::size_t remaining_space = capacity_ - offset_;
    char *current_ptr = buffer_ + offset_;
    void *aligned_ptr = current_ptr;

    if (std::align(alignment, size, aligned_ptr, remaining_space) ==
        nullptr) { // Alignment failed, we didn't have enough space
        // Alternatively:
        // return nullptr;
        throw std::bad_alloc();
    }

    // Otherwise, we go ahead and make our adjustments
    offset_ = static_cast<char *>(aligned_ptr) + size - buffer_;
    return aligned_ptr;
}

void ArenaAllocator::reset() { offset_ = 0; }
