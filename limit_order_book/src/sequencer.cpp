#include <atomic>
#include <sequencer.hpp>

// Instantiate counters with initial capcity
Sequencer::Sequencer(std::size_t capacity) : capacity(capacity) {
    counters = std::make_unique<std::atomic<std::uint64_t>[]>(capacity);
}

std::uint32_t Sequencer::next(std::size_t id) const {
    // We can have an initial check to see if the id exists essentially
    return counters[id].fetch_add(1, std::memory_order_relaxed);
}
