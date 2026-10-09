#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

// Bump allocator over a chain of fixed-size blocks. Allocation is a pointer
// increment; memory is only returned on reset() or destruction. When a block
// fills, a new block of the same size is appended, so the arena never throws
// for capacity (only if the OS refuses memory). Pages are not touched until
// used, so a large block size costs virtual space only.
class ArenaAllocator {
  public:
    ArenaAllocator(ArenaAllocator &&) = delete;
    ArenaAllocator(const ArenaAllocator &) = delete;
    ~ArenaAllocator();

    explicit ArenaAllocator(std::size_t block_size);

    void *allocate(std::size_t size, std::size_t alignment);

    // Rewind to the first block; extra blocks are released.
    void reset();

    std::size_t blocks() const { return blocks_.size(); }
    std::size_t block_size() const { return block_size_; }

  private:
    void add_block();

    std::vector<char *> blocks_;
    std::size_t block_size_;
    char *current_; // start of the current block
    std::size_t offset_;
};

// Typed free-list pool on top of an arena. NodeType must provide
// `NodeType *free_next_` and `void Clear()`.
template <typename NodeType> class ArenaPool {
  public:
    ArenaPool(ArenaAllocator &arena)
        : arena_(arena), next_available_(nullptr) {}

    template <typename... Args> NodeType *allocate(Args &&...args) {
        if (next_available_) { // reuse a freed node
            NodeType *node = next_available_;
            next_available_ = next_available_->free_next_;
            node->Clear();
            return new (node) NodeType(std::forward<Args>(args)...);
        }
        void *aligned_ptr = arena_.allocate(sizeof(NodeType), alignof(NodeType));
        return new (aligned_ptr) NodeType(std::forward<Args>(args)...);
    }

    void reset() {
        arena_.reset();
        next_available_ = nullptr;
    }

    void deallocate(NodeType *node) {
        node->~NodeType();
        node->free_next_ = next_available_;
        next_available_ = node;
    }

  private:
    ArenaAllocator &arena_;
    NodeType *next_available_;
};
