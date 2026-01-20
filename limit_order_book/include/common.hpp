#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// OrderBook Types
using Timestamp = uint64_t;
using Price = uint64_t;
using Quantity = uint64_t;
using OrderId = uint64_t;
enum Side { Buy, Sell };

// Event Architecture Types
using OrderRefNumber = uint64_t;
using MatchNumber = uint64_t;

using SymbolId = uint64_t;
using Symbol = std::string;

// Common reusable functions
inline Timestamp get_current_timestamp() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}
