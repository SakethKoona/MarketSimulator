#pragma once

#include <cstdint>

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
