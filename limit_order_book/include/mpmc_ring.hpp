#pragma once
// Bounded lock-free multi-producer / multi-consumer queue (Vyukov's
// sequence-slot design). Used where many ingress sessions submit to one
// shard, and where several shard threads report to one session. Never
// blocks: try_push returns false when full.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

template <typename T> class MpmcRing {
    struct Slot {
        std::atomic<std::size_t> seq;
        alignas(T) unsigned char storage[sizeof(T)];
        T *ptr() { return std::launder(reinterpret_cast<T *>(storage)); }
    };
    static constexpr std::size_t kCacheLine = 64;

  public:
    explicit MpmcRing(std::size_t capacity_pow2)
        : capacity_(capacity_pow2), mask_(capacity_pow2 - 1), slots_(new Slot[capacity_pow2]) {
        if (capacity_pow2 == 0 || (capacity_pow2 & mask_) != 0)
            throw std::bad_alloc();
        for (std::size_t i = 0; i < capacity_; ++i)
            slots_[i].seq.store(i, std::memory_order_relaxed);
    }
    ~MpmcRing() {
        T tmp;
        while (try_pop(tmp)) {
        }
        delete[] slots_;
    }
    MpmcRing(const MpmcRing &) = delete;
    MpmcRing &operator=(const MpmcRing &) = delete;

    template <typename U> bool try_push(U &&v) noexcept {
        std::size_t pos = tail_.load(std::memory_order_relaxed);
        for (;;) {
            Slot &s = slots_[pos & mask_];
            std::size_t seq = s.seq.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0) {
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    ::new (static_cast<void *>(s.storage)) T(std::forward<U>(v));
                    s.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false; // full
            } else {
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    bool try_pop(T &out) noexcept {
        std::size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot &s = slots_[pos & mask_];
            std::size_t seq = s.seq.load(std::memory_order_acquire);
            std::intptr_t diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = std::move(*s.ptr());
                    s.ptr()->~T();
                    s.seq.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = head_.load(std::memory_order_relaxed);
            }
        }
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }
    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

  private:
    const std::size_t capacity_;
    const std::size_t mask_;
    Slot *const slots_;
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    alignas(kCacheLine) std::atomic<std::uint64_t> dropped_{0};
};
