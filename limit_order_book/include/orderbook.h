
#include "skiplist.h"
#include <chrono>
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
  TypeInForce typeInForce;
  Side side;

  Order(OrderId orderId, Price price, Quantity quantity, OrderType orderType,
        Timestamp timestamp, TypeInForce typeInForce, Side side);
};

struct PriceLevel {
  Price price;
  std::deque<Order> orders;

  bool addOrder(Order order);
  bool removeOrder(OrderId orderId);
};

class OrderBook {
public:
  SkipList<Price, PriceLevel> getBids();
  SkipList<Price, PriceLevel> getAsks();
  bool addOrder(Order order);

private:
  SkipList<Price, PriceLevel> bids_;
  SkipList<Price, PriceLevel> asks_;
};
