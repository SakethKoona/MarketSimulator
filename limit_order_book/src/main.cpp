#include "../include/orderbook.hpp"
#include <iostream>
#include <ostream>

int main(int argc, char *argv[]) {

  OrderBook ob{};

  Order exampleOrder =
      Order(1, 40, 5, OrderType::LIMIT, TypeInForce::GTC, Side::Buy); // Buy @ 40
  Order anotherOrder =
      Order(2, 40, 10, OrderType::LIMIT, TypeInForce::GTC, Side::Buy); // Buy @ 40
  Order orderThree = 
      Order(3, 41, 15, OrderType::LIMIT, TypeInForce::GTC, Side::Buy); // Buy @ 41
  Order sellOrder =
      Order(4, 45, 20, OrderType::LIMIT, TypeInForce::GTC, Side::Sell); // Sell @ 45



  ob.addOrder(exampleOrder);
  ob.addOrder(orderThree);
  ob.addOrder(sellOrder);
  ob.addOrder(anotherOrder);

  

  ob.Display();

  return 0;
}
