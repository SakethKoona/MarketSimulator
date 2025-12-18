
#include "errors.hpp"
#include "skiplist.hpp"
#include <cstdint>
#include <list>
#include <unordered_map>

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
        TypeInForce typeInForce, Side side);
};

using OrderIterator = std::list<Order>::iterator;

struct PriceLevel {
  Price price;
  std::list<Order> orders;
  int size_;

  OrderIterator addOrder(const Order &order);
  OrderResult removeOrder(OrderIterator orderIt);
  int GetSize();
};

using Book = SkipList<Price, PriceLevel>;

struct OrderInfo {
  PriceLevel *priceLevel;
  std::list<Order>::iterator order;
};

class OrderBook {
  Book bids_;
  Book asks_;
  std::unordered_map<OrderId, OrderInfo> orderLookup_;

public:
  OrderBook();
  Book getBids();
  Book getAsks();
  OrderResult addOrder(Order order);
  OrderResult cancelOrder(OrderId id);
  void DisplayBook();

  PriceLevel *bestBid();
  PriceLevel *bestAsk();

private:
};
