#include <atomic>
#include <sequencer.hpp>

// Instantiate counters with initial capcity
Sequencer::Sequencer(std::size_t initial_capacity)
    : initial_cap(initial_capacity) {

    counters.reserve(initial_cap);
    for (std::size_t i = 0; i < initial_capacity; i++) {
        counters.emplace_back(0);
    }
}

std::uint32_t Sequencer::next(std::size_t id) {
    // We can have an initial check to see if the id exists essentially
    return counters[id].fetch_add(1, std::memory_order_relaxed);
}
