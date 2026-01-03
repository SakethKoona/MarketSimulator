#include "../include/engine.hpp"
#include <iostream>
#include <ostream>

int main() {

  // OrderBook ob = OrderBook("AAPL");

  // Order exampleOrder =
  //     Order(1, 40, 5, OrderType::LIMIT, TypeInForce::GTC, Side::Buy); // Buy @ 40
  // Order anotherOrder =
  //     Order(2, 40, 10, OrderType::LIMIT, TypeInForce::GTC, Side::Buy); // Buy @ 40
  // Order orderThree =
  //     Order(3, 41, 15, OrderType::LIMIT, TypeInForce::GTC, Side::Buy); // Buy @ 41
  // Order sellOrder =
  //     Order(4, 45, 20, OrderType::LIMIT, TypeInForce::GTC, Side::Sell); // Sell @ 45


  // ob.addOrder(exampleOrder);
  // ob.addOrder(orderThree);
  // ob.addOrder(sellOrder);
  // ob.addOrder(anotherOrder);

  // ob.Display();

  // // Okay, so for Cancel Order, the price level isn't being deleted if it's empty
  // ob.CancelOrder(orderThree.orderId);
  // auto res = ob.CancelOrder(orderThree.orderId);
  // ob.CancelOrder(sellOrder.orderId);

  // if (res == OrderResult::Success) {
  //   std::cout << "okay, something went wrong" << std::endl;
  // }

  // ob.Display();

  MatchingEngine engine{};

  OrderId id1 = engine.SubmitOrder("AAPL", 100, 5, Side::Buy);
  OrderId id2 = engine.SubmitOrder("AAPL", 105, 5, Side::Buy);

  engine.DisplayBook("AAPL");

  OrderId id3 = engine.SubmitOrder("AAPL", 100, 3, Side::Sell);
  engine.DisplayBook("AAPL");

  return 0;
}
