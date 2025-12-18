#include "../include/orderbook.hpp"
#include <iostream>
#include <ostream>

int main (int argc, char *argv[]) {

  OrderBook ob{};

  Order exampleOrder = Order(1, 2399, 5, OrderType::LIMIT, TypeInForce::GTC, Side::Buy);
  ob.addOrder(exampleOrder);
  return 0;
}
