
#include "errors.hpp"
#include "skiplist.hpp"
#include <cstdint>
#include <list>
#include <unordered_map>
#include <optional>

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


namespace COLORS {
    constexpr const char* reset     = "\033[0m";
    constexpr const char* bold      = "\033[1m";
    constexpr const char* dim       = "\033[2m";
    constexpr const char* underline= "\033[4m";
    constexpr const char* inverse  = "\033[7m";

    constexpr const char* red       = "\033[31m";
    constexpr const char* green     = "\033[32m";
    constexpr const char* yellow    = "\033[33m";
    constexpr const char* blue      = "\033[34m";
    constexpr const char* magenta   = "\033[35m";
    constexpr const char* cyan      = "\033[36m";
}


constexpr int PRICE_W = 8;
constexpr int INDENT = 5;
constexpr int ORDER_W = 20;

void PrintOrderBookHeader(std::ostream& os);

std::ostream& operator<<(std::ostream& os, const Order& order);

using OrderIterator = std::list<Order>::iterator;

struct PriceLevel {
  Price price;
  std::list<Order> orders;
  int size_ = 0;

  OrderIterator AddOrder(const Order &order);
  OrderResult RemoveOrder(OrderIterator orderIt);
  int GetSize();
  void SetPrice(Price price);
};

std::ostream& operator<<(std::ostream& os, const PriceLevel& pl);

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
  std::string symbol;
  OrderBook();
  OrderBook(std::string symbol);
  const Book& bids() const;
  const Book& asks() const;
  OrderResult addOrder(const Order& order);
  OrderResult cancelOrder(OrderId id);
  ModifyResult ModifyOrder(OrderId id, Quantity newQty);
  void Display();

  const PriceLevel *bestBid() const;
  const PriceLevel *bestAsk() const;

private:
};
