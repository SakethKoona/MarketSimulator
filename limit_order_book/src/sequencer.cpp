#include <atomic>
#include <sequencer.hpp>

Sequencer::Sequencer(std::size_t initial_capacity) : counters{} {}

std::uint32_t Sequencer::next(std::size_t id) {
    return counters[id].fetch_add(1, std::memory_order_relaxed);
}
