
#include "skiplist.h"
#include <cstdint>
#include <deque>

using Timestamp = uint64_t;
using Price = uint64_t;
using Quantity = uint64_t;
using OrderId = uint64_t;

enum Side { Buy, Sell };

enum OrderType {
  MARKET,
  LIMIT,
};

enum TypeInForce {
  GTC, // Good-Till-Cancel
  FOK, // Fill-Or-Kill
  IOC, // Immediate-Or-Cancel
};

struct Order {
  OrderId orderId;
  Price price;
  Quantity quantity;
  OrderType orderType;
  Timestamp timestamp;
  Side side;
};

struct PriceLevel {
  uint64_t price;
  std::deque<Order> orders;
};

class OrderBook {
public:
  SkipList<uint64_t, PriceLevel> getBids() { return bids_; }
  SkipList<uint64_t, PriceLevel> getAsks() { return asks_; }

private:
  SkipList<uint64_t, PriceLevel> bids_;
  SkipList<uint64_t, PriceLevel> asks_;
};
