#include "../include/orderbook.hpp"
#include "tabulate/tabulate.hpp"
#include "tabulate/table.hpp"
#include <chrono>
#include <unordered_map>
#include <iomanip>
#include <sstream>
#include <ctime>

/* ============================================================
   TIMESTAMP HELPERS
   ============================================================ */

inline void print_timestamp(std::ostream& os, std::int64_t ns_since_epoch) {
    using namespace std::chrono;

    nanoseconds ns{ns_since_epoch};
    seconds s = duration_cast<seconds>(ns);
    nanoseconds rem = ns - s;

    std::time_t tt = s.count();
    std::tm tm = *std::localtime(&tt);

    os << std::put_time(&tm, "%H:%M:%S")
       << '.'
       << std::setw(9) << std::setfill('0') << rem.count()
       << std::setfill(' ');
}

inline void print_timestamp_5dp(std::ostream& os, std::int64_t ns_since_epoch) {
    using namespace std::chrono;

    nanoseconds ns{ns_since_epoch};
    seconds s = duration_cast<seconds>(ns);

    auto frac = duration_cast<microseconds>(ns - s).count() / 10;

    std::time_t tt = s.count();
    std::tm tm = *std::localtime(&tt);

    os << std::put_time(&tm, "%H:%M:%S")
       << '.'
       << std::setw(5) << std::setfill('0') << frac
       << std::setfill(' ');
}

/* ============================================================
   HEADER
   ============================================================ */

void PrintOrderBookHeader(std::ostream& os) {
    constexpr int COL_W = 40;

    os << COLORS::bold
       << std::setw(COL_W) << "SELL"
       << " | "
       << std::setw(COL_W) << "BUY"
       << COLORS::reset
       << '\n';

    os << std::string(COL_W, '-')
       << "-+-"
       << std::string(COL_W, '-')
       << '\n';
}

/* ============================================================
   ORDER
   ============================================================ */

Order::Order(OrderId orderId, Price price, Quantity quantity,
             OrderType orderType, TypeInForce typeInForce, Side side)
    : orderId(orderId), price(price), quantity(quantity), orderType(orderType),
      typeInForce(typeInForce), side(side) {

    this->timestamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();
}

std::ostream& operator<<(std::ostream& os, const Order& o) {
    os << COLORS::dim << "[";
    print_timestamp_5dp(os, o.timestamp);
    os << "]" << COLORS::reset << " "
       << COLORS::cyan << "O" << o.orderId << COLORS::reset << " | ";

    if (o.side == Side::Buy) {
        os << COLORS::green << "BUY ";
    } else {
        os << COLORS::red << "SELL ";
    }

    os << COLORS::reset
       << o.quantity << " @ ";

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

/* ============================================================
   ORDER BOOK
   ============================================================ */

OrderBook::OrderBook() : bids_(0.5f), asks_(0.5f) {}

Book OrderBook::getBids() { return bids_; }
Book OrderBook::getAsks() { return asks_; }

OrderResult OrderBook::addOrder(Order order) { // unchanged
    if (order.quantity <= 0)
        return OrderResult::InvalidQty;

    auto it = orderLookup_.find(order.orderId);
    if (it != orderLookup_.end())
        return OrderResult::DuplicateOrder;

    auto &book = (order.side == Side::Buy) ? bids_ : asks_;
    auto *priceLevel = book.insertOrGet(order.price);
    priceLevel->value.SetPrice(order.price);
    auto insertResult = priceLevel->value.addOrder(order);

    OrderInfo entryInfo = OrderInfo{};
    entryInfo.priceLevel = &priceLevel->value;
    entryInfo.order = insertResult;

    orderLookup_.insert({order.orderId, entryInfo});

    return OrderResult::Success;
}

OrderResult OrderBook::cancelOrder(OrderId id) { // unchanged
    auto it = orderLookup_.find(id);
    if (it == orderLookup_.end())
        return OrderResult::OrderNotFound;

    PriceLevel* priceLevel = it->second.priceLevel;
    priceLevel->removeOrder(it->second.order);

    if (priceLevel->GetSize() <= 0) {
        auto &book = it->second.order->side == Side::Buy ? bids_ : asks_;
        book.delete_node(priceLevel->price);
    }

    orderLookup_.erase(it);
    return OrderResult::Success;
}

PriceLevel *OrderBook::bestAsk() {
    return &asks_.head_ptr->forward[0]->value;
}

PriceLevel *OrderBook::bestBid() {
    return &bids_.head_ptr->forward[0]->value;
}

/* ============================================================
   DISPLAY (L3, FORWARD ONLY)
   ============================================================ */

void OrderBook::Display() {
    using namespace tabulate;

    Table table;

    table.add_row({"SELL ORDERS", "BUY ORDERS"});
    table.row(0).format()
        .font_style({FontStyle::bold})
        .font_color(Color::cyan);

    auto* bids_it = bids_.head_ptr->forward[0];
    auto* asks_it = asks_.head_ptr->forward[0];


    while (bids_it && asks_it) {
        std::cout << "BID LEVEL: " << std::endl;
        std::cout << bids_it->key << bids_it->value.GetSize() << std::endl;

        std::cout << "ASK LEVEL: " << std::endl;
        std::cout << asks_it->key << asks_it->value.GetSize() << std::endl;
        

        bids_it = bids_it->forward[0];
        asks_it = asks_it->forward[0];
    }

    std::cout << table << std::endl;
}

