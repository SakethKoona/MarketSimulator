#include "../include/orderbook.h"
#include <chrono>

SkipList<Price, PriceLevel> OrderBook::getBids() { return bids_; }

SkipList<Price, PriceLevel> OrderBook::getAsks() { return asks_; }

Order::Order(OrderId orderId, Price price, Quantity quantity,
             OrderType orderType, Timestamp timestamp, TypeInForce typeInForce,
             Side side)
    : orderId(orderId), price(price), quantity(quantity), orderType(orderType),
      typeInForce(typeInForce), side(side) {

  this->timestamp =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();
}

Order* PriceLevel::addOrder(Order order) {
  // TODO: Do any necessary preprocessing here
  // if quantity < 0, then we return false for success
  if (order.quantity <= 0) {
    return nullptr;
  }

  orders.push_back(order);
  return &order;
}

bool OrderBook::addOrder(Order order) {
  auto &book = (order.side == Side::Buy) ? bids_ : asks_;
  // First, we either insert or get the existing price level within the book
  // Instead of inserting the order here, we would basicaly insert an empty
  // price level or should we insert the pre-created price level with the order
  // already in it.
  // 1. when we call insertOrGet, and the its get, we pass in an unnecessary
  // value, when we just need the key, so it creates an unnecessary node in
  // memory without inserting it anywhere,
  // 2. if we need insert, then we have to create a price level
  // Okay, I fixed it by making the insert method insertOrGet, so it either
  // returns the existing one or creates a new pointer and returns that, and
  // then we can just add to that directly
  auto *level = book.insertOrGet(order.price);
  level->value.addOrder(order);
  return true;
}

bool OrderBook::cancelOrder(OrderId id) {
  

  return true;
}


PriceLevel *OrderBook::bestAsk() {
  return &asks_.head_ptr->forward[0]->value;
} // O(1)
PriceLevel *OrderBook::bestBid() { return &bids_.getMax()->value; } // O(1)
