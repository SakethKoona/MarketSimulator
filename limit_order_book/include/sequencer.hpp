#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

// Design for the sequencer:
// - we want this to store multiple atomic incrementable values
// - and for us to access them and increment them in O(1) time
// - initially, I thought that we could just store an unordered map with String
// -> values
//
// - but that's a string in the hotpath, and we don't want that
// - so how would we access and store information to differentiate
// - and get different id sequences

class Sequencer {
  public:
    Sequencer(std::size_t initial_capacity);
    std::uint32_t next(std::size_t id) const;

  private:
    std::unique_ptr<std::atomic<std::uint64_t>[]> counters;
    std::size_t capacity;
};
