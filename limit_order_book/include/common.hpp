#pragma once

#include <chrono>
#include <cstdint>
#include <string>

// OrderBook Types
using Timestamp = uint64_t;
using Price = uint64_t;
using Quantity = uint64_t;
using OrderId = uint64_t;
enum Side : uint8_t { Buy, Sell };

// Event Architecture Types
using OrderRefNumber = uint64_t;
using MatchNumber = uint64_t;

using SymbolId = uint64_t;
using Symbol = std::string;
using TradeId = uint64_t;
// Wall-clock nanoseconds since the Unix epoch. Used for everything that
// leaves the process (events, order timestamps). high_resolution_clock is
// steady_clock on libc++ (ns since boot), so it is wrong for a feed.
inline Timestamp get_current_timestamp() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Monotonic nanoseconds, for latency measurement only.
inline Timestamp get_monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
