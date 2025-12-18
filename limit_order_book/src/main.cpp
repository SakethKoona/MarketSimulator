#include "../include/orderbook.hpp"
#include <iostream>
#include <ostream>

int main(int argc, char *argv[]) {

  OrderBook ob{};

  Order exampleOrder =
      Order(1, 2399, 5, OrderType::LIMIT, TypeInForce::GTC, Side::Buy);
  auto result = ob.addOrder(exampleOrder);
  switch (result) {
  case OrderResult::Success:
    std::cout << "YES, IT FUCKING WORKS" << std::endl;
    break;
  default:
    std::cout << "WE FAILED :(" << std::endl;
  }

  switch (ob.cancelOrder(2)) {
  case OrderResult::Success:
    std::cout << "WORKED AGAIN HAHA" << std::endl;
    break;
  default:
    std::cout << "WE DID SOMETHING WRONG" << std::endl;
  }

  return 0;
}
