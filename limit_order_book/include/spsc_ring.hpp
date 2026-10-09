#pragma once
// Bounded lock-free single-producer / single-consumer ring.
//
// Exactly one thread may call try_push and exactly one may call try_pop /
// peek / pop. Capacity must be a power of two. The ring never blocks: when
// full, try_push returns false and bumps a drop counter so the producer (the
// matching engine) is never stalled by a slow consumer (the publisher).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

template <typename T> class SpscRing {
    static constexpr std::size_t kCacheLine = 64;

  public:
    explicit SpscRing(std::size_t capacity_pow2)
        : capacity_(capacity_pow2), mask_(capacity_pow2 - 1),
          slots_(static_cast<T *>(::operator new(
              sizeof(T) * capacity_pow2, std::align_val_t{alignof(T)}))) {
        if (capacity_pow2 == 0 || (capacity_pow2 & mask_) != 0) {
            ::operator delete(slots_, std::align_val_t{alignof(T)});
            throw std::bad_alloc(); // not a power of two
        }
    }

    ~SpscRing() {
        std::size_t h = head_.load(std::memory_order_relaxed);
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        while (h != t) {
            slots_[h & mask_].~T();
            ++h;
        }
        ::operator delete(slots_, std::align_val_t{alignof(T)});
    }

    SpscRing(const SpscRing &) = delete;
    SpscRing &operator=(const SpscRing &) = delete;

    // Producer side.
    template <typename U> bool try_push(U &&v) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - cached_head_ == capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ == capacity_) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        ::new (static_cast<void *>(&slots_[t & mask_])) T(std::forward<U>(v));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Front element or nullptr; valid until the next pop().
    T *peek() noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (h == cached_tail_)
                return nullptr;
        }
        return &slots_[h & mask_];
    }

    // Consumer side. Removes the front element (must exist).
    void pop() noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        slots_[h & mask_].~T();
        head_.store(h + 1, std::memory_order_release);
    }

    // Consumer side. Moves the front element out and pops it.
    bool try_pop(T &out) noexcept {
        T *p = peek();
        if (!p)
            return false;
        out = std::move(*p);
        pop();
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    // Approximate from any thread; exact from the consumer thread.
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire);
    }
    std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

  private:
    const std::size_t capacity_;
    const std::size_t mask_;
    T *const slots_;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0}; // consumer owns
    alignas(kCacheLine) std::size_t cached_tail_{0};       // consumer only
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0}; // producer owns
    alignas(kCacheLine) std::size_t cached_head_{0};       // producer only
    alignas(kCacheLine) std::atomic<std::uint64_t> dropped_{0};
};
