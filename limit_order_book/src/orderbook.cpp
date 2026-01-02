#include "../include/orderbook.hpp"
#include <iomanip>
#include <sstream>

#ifndef DEBUG
  #define DEBUG
#endif

/* ============================================================
   TIMESTAMP HELPERS
   ============================================================ */

inline void print_timestamp(std::ostream &os, std::int64_t ns_since_epoch) {
  using namespace std::chrono;

  nanoseconds ns{ns_since_epoch};
  seconds s = duration_cast<seconds>(ns);
  nanoseconds rem = ns - s;

  std::time_t tt = s.count();
  std::tm tm = *std::localtime(&tt);

  os << std::put_time(&tm, "%H:%M:%S") << '.' << std::setw(9)
     << std::setfill('0') << rem.count() << std::setfill(' ');
}

inline void print_timestamp_5dp(std::ostream &os, std::int64_t ns_since_epoch) {
  using namespace std::chrono;

  nanoseconds ns{ns_since_epoch};
  seconds s = duration_cast<seconds>(ns);

  auto frac = duration_cast<microseconds>(ns - s).count() / 10;

  std::time_t tt = s.count();
  std::tm tm = *std::localtime(&tt);

  os << std::put_time(&tm, "%H:%M:%S") << '.' << std::setw(5)
     << std::setfill('0') << frac << std::setfill(' ');
}

/* ============================================================
   HEADER
   ============================================================ */

void PrintOrderBookHeader(std::ostream &os) {
  constexpr int COL_W = 40;

  os << COLORS::bold << std::setw(COL_W) << "SELL"
     << " | " << std::setw(COL_W) << "BUY" << COLORS::reset << '\n';

  os << std::string(COL_W, '-') << "-+-" << std::string(COL_W, '-') << '\n';
}

/* ============================================================
   ORDER
   ============================================================ */

Order::Order(OrderId orderId, Price price, Quantity quantity,
             OrderType orderType, TypeInForce typeInForce, Side side)
    : orderId(orderId), price(price), quantity(quantity), orderType(orderType),
      typeInForce(typeInForce), side(side) { // TODO: Add functionality here that automatically creates an order id

  this->timestamp =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::high_resolution_clock::now().time_since_epoch())
          .count();
}

std::ostream &operator<<(std::ostream &os, const Order &o) {
  os << COLORS::dim << "[";
  print_timestamp_5dp(os, o.timestamp);
  os << "]" << COLORS::reset << " " << COLORS::cyan << "O" << o.orderId
     << COLORS::reset << " | ";

  if (o.side == Side::Buy) {
    os << COLORS::green << "BUY ";
  } else {
    os << COLORS::red << "SELL ";
  }

  os << COLORS::reset << o.quantity << " @ ";

  if (o.orderType == OrderType::LIMIT) {
    os << COLORS::yellow << o.price;
  } else {
    os << COLORS::magenta << "MARKET";
  }

  return os << COLORS::reset;
}

/* ============================================================
   PRICE LEVEL
   ============================================================ */

OrderIterator PriceLevel::AddOrder(const Order &order) {
  orders.push_back(order);
  size_++;
  totalQuantity += order.quantity;
  auto it = orders.end();
  return --it;
}

OrderResult PriceLevel::RemoveOrder(OrderIterator orderIt) {
  totalQuantity -= orderIt->quantity;
  orders.erase(orderIt);
  size_--;
  return OrderResult::Success;
}

ModifyResult PriceLevel::ModifyOrder(OrderIterator& orderIt, Quantity newQty) {
  if (newQty > orderIt->quantity) return ModifyResult::Rejected;
  if (newQty == 0) {
    RemoveOrder(orderIt);
    return ModifyResult::Success;
  }
  totalQuantity -= (orderIt->quantity - newQty);
  orderIt->quantity = newQty;
  return ModifyResult::Success;
}

int PriceLevel::GetSize() { return size_; }

Quantity PriceLevel::TotalQuantity() const { return totalQuantity; }

void PriceLevel::SetPrice(Price price) { this->price = price; }

std::ostream& operator<<(std::ostream& os, const PriceLevel& pl) {
  os << COLORS::magenta << pl.price << COLORS::reset << std::endl;

  for (const auto& order : pl.orders) {
    os << "  " << order << std::endl;
  }

  return os;
}

/* ============================================================
   ORDER BOOK
   ============================================================ */

OrderBook::OrderBook() 
    : bids_(0.5), asks_(0.5), symbol("") {
}

OrderBook::OrderBook(const std::string& sym) 
    : bids_(0.5), asks_(0.5), symbol(sym) {
}

const Book& OrderBook::bids() const { return bids_; }
const Book& OrderBook::asks() const { return asks_; }

OrderResult OrderBook::addOrder(const Order& order) {
  // Small issue here to fix later TODO: we set price even if we get an existing price level
  // And apparently that might break invariants in the future, so something to watch out for
  if (order.quantity <= 0)
    return OrderResult::InvalidQty;

  auto it = orderLookup_.find(order.orderId);
  if (it != orderLookup_.end())
    return OrderResult::DuplicateOrder;

  // Checks have passed do the actual inserting
  auto &book = (order.side == Side::Buy) ? bids_ : asks_;
  Price priceKey = (order.side == Side::Buy) ? -order.price : order.price;
  auto *priceLevel = book.insertOrGet(priceKey); // Create a new Price level if needed or get the old one
  priceLevel->value.SetPrice(order.price); // When we set the price in the price level, we keep it positive, since we only get by Key
  auto insertResult = priceLevel->value.AddOrder(order);

  OrderInfo entryInfo = OrderInfo{};
  entryInfo.priceLevel = &priceLevel->value;
  entryInfo.order = insertResult;

  orderLookup_.insert({order.orderId, entryInfo});

  return OrderResult::Success;
}

OrderResult OrderBook::CancelOrder(OrderId id) {
  auto it = orderLookup_.find(id);
  if (it == orderLookup_.end())
    return OrderResult::OrderNotFound;

  // Delete FROM the price level
  PriceLevel *priceLevel = it->second.priceLevel;
  Side side = it->second.order->side;
  priceLevel->RemoveOrder(it->second.order);


  // Delete the price level if needed
  // TODO: Add error handling here if node deletion fails
  if (priceLevel->GetSize() <= 0) {
    auto &book = side == Side::Buy ? bids_ : asks_;
    Price priceKey = (side == Side::Buy) ? -priceLevel->price : priceLevel->price;
    book.delete_node(priceKey);
  }

  // If all went well, delete from the orderLookup_
  orderLookup_.erase(it);
  return OrderResult::Success;
}

ModifyResult OrderBook::ModifyOrder(OrderId id, Quantity newQty) {
  // First, we find the order within the orderlookup
  auto it = orderLookup_.find(id);
  if (it == orderLookup_.end()) {
    return ModifyResult::OrderNotFound;
  }

  // Get the pointer to the actual order
  OrderInfo& entryInfo = it->second;
  PriceLevel* pl = entryInfo.priceLevel;
  OrderIterator order = entryInfo.order;

  ModifyResult res =  pl->ModifyOrder(order, newQty); // We remove the order here and update the size if the new quantity is 0
  
  if (newQty == 0 || (res == ModifyResult::Success && pl->GetSize() == 0)) {
    orderLookup_.erase(id);
    auto& book = order->side == Side::Buy ? bids_ : asks_;
    if (pl->GetSize() == 0) book.delete_node(pl->price);
  }

  return res;
}

const PriceLevel *OrderBook::bestAsk() const {
  auto* node = asks_.head_ptr->forward[0];
  if (!node) return nullptr;
  return &node->value;
}

const PriceLevel *OrderBook::bestBid() const {
  auto* node = bids_.head_ptr->forward[0];
  if (!node) return nullptr;
  return &node->value;
}

/* ============================================================
   DISPLAY (L3, FORWARD ONLY)
   ============================================================ */

bool isDarkMode() {
  const char* colorterm = std::getenv("COLORFG");
  if (colorterm && std::string(colorterm) == "light") {
    return false;
  }

  // Check if running in common dark mode terminals
  const char* term = std::getenv("TERM_PROGRAM");
  if (term) {
    std::string termStr(term);
    if (termStr == "iTerm.app" || termStr == "Apple_Terminal") {
      // Most macOS terminals default to dark mode
      return true;
    }
  }

  // Default to dark mode
  return true;
}

void OrderBook::Display() {
  bool darkMode = isDarkMode();

  // Choose colors based on mode
  std::string askHeaderColor = darkMode ? COLORS::red : COLORS::red;
  std::string bidHeaderColor = darkMode ? COLORS::green : COLORS::green;
  std::string priceColor = darkMode ? COLORS::yellow : COLORS::magenta;
  std::string boxColor = darkMode ? COLORS::bold : COLORS::dim;

  // Display symbol header
  std::cout << "\n" << COLORS::bold << COLORS::cyan
            << "════════════════════════════════════════\n"
            << "         ORDER BOOK: " << symbol << "\n"
            << "════════════════════════════════════════"
            << COLORS::reset << "\n";

  // Display ASKS
  std::cout << "\n" << boxColor << askHeaderColor
            << "╔═══════════════════════════════════════╗\n"
            << "║            SELL SIDE (ASKS)           ║\n"
            << "╚═══════════════════════════════════════╝"
            << COLORS::reset << "\n\n";

  auto* askNode = asks_.head_ptr->forward[0];
  while (askNode) {
    PriceLevel& level = askNode->value;

    std::cout << priceColor << COLORS::bold
              << "Price: $" << level.price
              << " (" << level.GetSize() << " orders)"
              << COLORS::reset << "\n";

    for (const auto& order : level.orders) {
      std::cout << "  " << order << "\n";
    }
    std::cout << "\n";

    askNode = askNode->forward[0];
  }

  // Display BIDS
  std::cout << boxColor << bidHeaderColor
            << "╔═══════════════════════════════════════╗\n"
            << "║            BUY SIDE (BIDS)            ║\n"
            << "╚═══════════════════════════════════════╝"
            << COLORS::reset << "\n\n";

  auto* bidNode = bids_.head_ptr->forward[0];
  while (bidNode) {
    PriceLevel& level = bidNode->value;

    std::cout << priceColor << COLORS::bold
              << "Price: $" << level.price
              << " (" << level.GetSize() << " orders)"
              << COLORS::reset << "\n";

    for (const auto& order : level.orders) {
      std::cout << "  " << order << "\n";
    }
    std::cout << "\n";

    bidNode = bidNode->forward[0];
  }
}

const OrderInfo* OrderBook::FindOrder(OrderId id) {
  auto it = orderLookup_.find(id);
  return (it == orderLookup_.end()) ?  nullptr : &it->second;
}
