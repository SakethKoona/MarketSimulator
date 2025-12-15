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
  // TODO: Do any necessary preprocessing here
  orders.push_front(order);

  return true;
}

bool OrderBook::addOrder(Order order) {
  if (order.side == Side::Buy) {
    // We add it to the bids_
    // First, we want to check whether that price level exists
    auto *priceLevel = bids_.search(order.price);
    if (priceLevel == nullptr) {
      // The price level node doesn't exist, we have a unique price, so we want to add a new node
      PriceLevel newPriceLevel;
      newPriceLevel.price = order.price;
      newPriceLevel.addOrder(order);

      bids_.insert(order.price, newPriceLevel);
    } else {
      // Otherwise, we simply add the new order to that already existing price level
      priceLevel->value.addOrder(order);
    } 
  }
}

