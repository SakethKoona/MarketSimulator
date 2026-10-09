#include <atomic>
#include <sequencer.hpp>

// Instantiate counters with initial capcity
Sequencer::Sequencer(std::size_t capacity) : capacity(capacity) {
    counters = std::make_unique<std::atomic<std::uint64_t>[]>(capacity);
    // Sequences start at 1; 0 is reserved as the "no id" sentinel
    for (std::size_t i = 0; i < capacity; i++)
        counters[i].store(1, std::memory_order_relaxed);
}

std::size_t Sequencer::size() const { return capacity; }

std::uint64_t Sequencer::next(std::size_t id) const {
    // We can have an initial check to see if the id exists essentially
    return counters[id].fetch_add(1, std::memory_order_relaxed);
}
