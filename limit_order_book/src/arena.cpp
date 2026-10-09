#include "arena.hpp"

ArenaAllocator::ArenaAllocator(std::size_t block_size)
    : block_size_(block_size), current_(nullptr), offset_(0) {
    add_block();
}

ArenaAllocator::~ArenaAllocator() {
    for (char *b : blocks_)
        ::operator delete(b);
}

void ArenaAllocator::add_block() {
    char *b = static_cast<char *>(::operator new(block_size_));
    blocks_.push_back(b);
    current_ = b;
    offset_ = 0;
}

void *ArenaAllocator::allocate(std::size_t size, std::size_t alignment) {
    if (size + alignment > block_size_)
        throw std::bad_alloc(); // single object larger than a block

    std::size_t remaining = block_size_ - offset_;
    void *ptr = current_ + offset_;
    if (std::align(alignment, size, ptr, remaining) == nullptr) {
        add_block();
        remaining = block_size_;
        ptr = current_;
        std::align(alignment, size, ptr, remaining); // cannot fail now
    }

    offset_ = static_cast<char *>(ptr) + size - current_;
    return ptr;
}

void ArenaAllocator::reset() {
    for (std::size_t i = 1; i < blocks_.size(); i++)
        ::operator delete(blocks_[i]);
    blocks_.resize(1);
    current_ = blocks_[0];
    offset_ = 0;
}
