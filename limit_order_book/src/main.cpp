#include "../include/orderbook.hpp"
#include <iostream>
#include <ostream>

int main(int argc, char *argv[]) {

  OrderBook ob{};

  Order exampleOrder =
      Order(1, 40, 5, OrderType::LIMIT, TypeInForce::GTC, Side::Buy);
  Order anotherOrder =
      Order(2, 40, 10, OrderType::LIMIT, TypeInForce::GTC, Side::Buy);


  Order orderThree = Order(3, 41, 15, OrderType::LIMIT, TypeInForce::GTC, Side::Buy);



  ob.addOrder(exampleOrder);
  ob.addOrder(orderThree);
  
  auto result = ob.addOrder(anotherOrder);
  switch (result) {
  case OrderResult::Success:
    std::cout << "YES, IT FUCKING WORKS" << std::endl;
    break;
  default:
    std::cout << "WE FAILED :(" << std::endl;
  }

  ob.Display();

  return 0;
}
