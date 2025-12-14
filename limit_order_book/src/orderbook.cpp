#include "../include/orderbook.h"
#include <chrono>

SkipList<Price, PriceLevel> OrderBook::getBids() { return bids_; }

SkipList<Price, PriceLevel> OrderBook::getAsks() { return asks_; }

Order::Order(OrderId orderId, Price price, Quantity quantity,
             OrderType orderType, Timestamp timestamp, TypeInForce typeInForce,
             Side side)
    : orderId(orderId), price(price), quantity(quantity), orderType(orderType),
      typeInForce(typeInForce), side(side) {

  timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::high_resolution_clock::now().time_since_epoch())
                  .count();
}

bool PriceLevel::addOrder(Order order) {
  //TODO: Do any necessary preprocessing here
  orders.push_front(order);

  return true;
}

