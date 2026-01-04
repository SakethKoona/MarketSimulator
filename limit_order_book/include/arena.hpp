#pragma once
#include <cstddef>
#include <new>
#include <memory>

class ArenaAllocator {
public:
    // Delete Move and Copy constructors
    ArenaAllocator(ArenaAllocator&&) = delete;
    ArenaAllocator(const ArenaAllocator&) = delete;
    ~ArenaAllocator() = default;

    // Base constructor (default one)
    explicit ArenaAllocator(std::size_t size);

    void* allocate(std::size_t size, std::size_t alignment);
    void reset();

private:
    char* buffer_;
    std::size_t capacity_;
    std::size_t offset_;
};


template<typename NodeType> class ArenaPool {
public:
    ArenaPool(ArenaAllocator& arena) : arena_(arena), next_available_(nullptr) {}

    // Allocate memory (basically a wrapper)
    template<typename... Args>
    NodeType* allocate(Args&& ...args) {
        if (next_available_) { // If we have some available already allocated reusable memory
            NodeType* node = next_available_;
            next_available_ = next_available_->free_next_;

            node->Clear(); // Optional
            return new (node) NodeType(std::forward<Args>(args)...);
        } else { // Standard call to our allocator with construction obviously
            void* aligned_ptr = arena_.allocate(sizeof(NodeType), alignof(NodeType));
            return new (aligned_ptr) NodeType(std::forward<Args>(args)...);
        }
    }

    void reset() {
        arena_.reset();
    }

    void deallocate(NodeType* node) {
        node->~NodeType();

        node->free_next_ = next_available_;
        next_available_ = node;
    }


private:
    ArenaAllocator& arena_;
    NodeType* next_available_;
};