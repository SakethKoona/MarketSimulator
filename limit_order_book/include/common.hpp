#pragma once

#include <chrono>
#include <cstddef>
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
using ShardId = uint8_t;

// Shard-tagged 64-bit ids: top 8 bits = shard that minted the id, low 56
// bits = per-shard sequence starting at 1. Used for OrderId, TradeId and
// book_seq so each stays globally unique across shards without any shared
// counter. 0 is never a valid id, and an order can be routed to its shard
// with a shift and no lookup.
constexpr unsigned kShardBits = 8;
constexpr unsigned kSeqBits = 64 - kShardBits;
constexpr uint64_t kSeqMask = (uint64_t{1} << kSeqBits) - 1;
constexpr std::size_t kMaxShards = std::size_t{1} << kShardBits;

constexpr uint64_t make_shard_id(ShardId shard, uint64_t seq) {
    return (static_cast<uint64_t>(shard) << kSeqBits) | (seq & kSeqMask);
}
constexpr ShardId shard_of(uint64_t id) {
    return static_cast<ShardId>(id >> kSeqBits);
}
constexpr uint64_t seq_of(uint64_t id) { return id & kSeqMask; }

// OrderId-specific spelling of the above
constexpr OrderId make_order_id(ShardId shard, uint64_t seq) {
    return make_shard_id(shard, seq);
}
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
