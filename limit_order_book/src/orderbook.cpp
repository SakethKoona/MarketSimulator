#include "../include/orderbook.hpp"
#include <chrono>
#include <unordered_map>
#include <iomanip>



void PrintOrderBookHeader(std::ostream& os) {
    // Column titles
    os << std::setw(PRICE_W + ORDER_W - PRICE_W) << "SELL"    // left side
       << std::string(PRICE_W + ORDER_W - PRICE_W, ' ') << "|"
       << std::setw(PRICE_W + ORDER_W - PRICE_W) << "BUY"    // right side
       << '\n';

    // Separator line
    os << std::string(PRICE_W + ORDER_W, '-')   // SELL side
       << std::string(PRICE_W + ORDER_W - PRICE_W, '-')               // separator
       << std::string(PRICE_W + ORDER_W, '-')   // BUY side
       << '\n';
}




/*
ORDER METHODS
*/

// Constructor
Order::Order(OrderId orderId, Price price, Quantity quantity,
             OrderType orderType, TypeInForce typeInForce, Side side)
    : orderId(orderId), price(price), quantity(quantity), orderType(orderType),
      typeInForce(typeInForce), side(side) {

  this->timestamp =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();
}

// Order Display method
std::ostream& operator<<(std::ostream& os, const Order& o) {
    os << COLORS::dim << "[" << o.timestamp << "]" << COLORS::reset << " "
       << COLORS::cyan << "O" << o.orderId << COLORS::reset << " | ";

    if (o.side == Side::Buy) {
        os << COLORS::green << "BUY ";
    } else {
        os << COLORS::red << "SELL ";
    }

    os << COLORS::reset
       << o.quantity << " @ ";

    if (o.orderType == OrderType::LIMIT) {
        os << COLORS::yellow << "$" << o.price;
    } else {
        os << COLORS::magenta << "MARKET";
    }

    return os << COLORS::reset;
}


/*
PRICE LEVEL METHODS
*/
OrderIterator PriceLevel::addOrder(const Order &order) {
  orders.push_back(order);
  size_++;
  auto it = orders.end();
  return --it;
}

OrderResult PriceLevel::removeOrder(OrderIterator orderIt) {
  orders.erase(orderIt);
  size_--;
  return OrderResult::Success;
}

int PriceLevel::GetSize() { return size_; }

void PriceLevel::SetPrice(Price price) {
  this->price = price;
}

std::ostream& operator<<(std::ostream& os, const PriceLevel& pl) {

  os << COLORS::yellow << pl.price << COLORS::reset << std::endl;
  
  for(const auto& order: pl.orders) {
      os << std::setw(INDENT) << order << std::endl;
  }

  return os;
}

/*
ORDER BOOK METHODS
*/

OrderBook::OrderBook() : bids_(0.5f), asks_(0.5f) {}

Book OrderBook::getBids() { return bids_; }
Book OrderBook::getAsks() { return asks_; }

OrderResult OrderBook::addOrder(Order order) { // O(log P) -> O(1)
  // Validations
  if (order.quantity <= 0)
    return OrderResult::InvalidQty;

  auto it = orderLookup_.find(order.orderId);
  if (it != orderLookup_.end()) {
    return OrderResult::DuplicateOrder;
  }

  auto &book = (order.side == Side::Buy) ? bids_ : asks_;
  auto *priceLevel = book.insertOrGet(order.price);      // O(log N)
  priceLevel->value.SetPrice(order.price); // O(1)
  auto insertResult = priceLevel->value.addOrder(order); // O(1)

  OrderInfo entryInfo = OrderInfo{};
  entryInfo.priceLevel = &priceLevel->value;
  entryInfo.order = insertResult;

  orderLookup_.insert({order.orderId, entryInfo});

  return OrderResult::Success;
}

OrderResult OrderBook::cancelOrder(OrderId id) {
  auto it = orderLookup_.find(id); // Hashmap iterator
  if (it == orderLookup_
                .end()) { // The order doesn't exist, so we return OrderNotFound
    return OrderResult::OrderNotFound;
  } else {
    // Remove from the priceLevel
    PriceLevel *priceLevel = it->second.priceLevel;
    priceLevel->removeOrder(it->second.order);

    // Remove priceLevel if empty after deletion
    if (priceLevel->GetSize() <= 0) {
      // Remove the price level from the skiplist
      auto &book = it->second.order->side == Side::Buy ? bids_ : asks_;
      book.delete_node(priceLevel->price);
    }

    // Remove from order registry
    orderLookup_.erase(it);
  }

  return OrderResult::Success;
}

PriceLevel *OrderBook::bestAsk() {
  return &asks_.head_ptr->forward[0]->value;
} // O(1)

PriceLevel *OrderBook::bestBid() {
  return &bids_.getMax()->value;
} // O(1)

void OrderBook::Display() {
  /**
   * For this function we want to Display
   * both sides of the orderbook with all the orders that exist for each side
   * Or, do we?
   * What's the best way to display here?
   * Do we do something like an L2 Feed, this would be pretty simple
   * we'd just add a total quantity section to the price level and we can just
   * display that
   */

  PrintOrderBookHeader(std::cout);

  // Print out one example price level from each side for now

  auto bidsCurrent = bids_.head_ptr->forward[0];
  while (bidsCurrent)
  {
    std::cout << bidsCurrent->value << std::endl;
    bidsCurrent = bidsCurrent->forward[0];
  }
  
}
