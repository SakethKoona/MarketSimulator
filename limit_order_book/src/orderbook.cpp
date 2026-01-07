#include "../include/orderbook.hpp"
#include <iomanip>
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
      typeInForce(typeInForce), side(side) {

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

ModifyResult PriceLevel::ModifyOrder(OrderIterator &orderIt, Quantity newQty) {
    if (newQty > orderIt->quantity) {
        return ModifyResult::QtyIncreaseNotAllowed;
    }
    Quantity diff = orderIt->quantity - newQty;
    totalQuantity -= diff;
    orderIt->quantity = newQty;
    return ModifyResult::Success;
}

int PriceLevel::GetSize() { return size_; }

Quantity PriceLevel::TotalQuantity() const { return totalQuantity; }

void PriceLevel::SetPrice(Price price) { this->price = price; }

std::ostream &operator<<(std::ostream &os, const PriceLevel &pl) {
    os << COLORS::magenta << pl.price << COLORS::reset << std::endl;

    for (const auto &order : pl.orders) {
        os << "  " << order << std::endl;
    }

    return os;
}

/* ============================================================
   ORDER BOOK
   ============================================================ */

OrderBook::OrderBook() : bids_(0.5), asks_(0.5), symbol("") {}

OrderBook::OrderBook(const std::string &sym)
    : bids_(0.5), asks_(0.5), symbol(sym) {}

const Book &OrderBook::bids() const { return bids_; }
const Book &OrderBook::asks() const { return asks_; }

OrderResult OrderBook::AddOrder(const Order &order) {
    // WARN: Small issue here to fix later we set price even if we get an
    // existing price level And apparently that might break invariants in the
    // future, so something to watch out for
    //
    //
    // TODO: Is this really O(1) if we have an already existing price level
    if (order.quantity <= 0)
        return OrderResult::InvalidQty;

    auto it = orderLookup_.find(order.orderId);
    if (it != orderLookup_.end())
        return OrderResult::DuplicateOrder;

    // Checks have passed do the actual inserting
    auto &book = (order.side == Side::Buy) ? bids_ : asks_;
    Price priceKey = (order.side == Side::Buy) ? -order.price : order.price;
    auto *priceLevel = book.insertOrGet(
        priceKey); // Create a new Price level if needed or get the old one
    priceLevel->value.SetPrice(
        order.price); // When we set the price in the price level, we keep it
                      // positive, since we only get by Key
    auto insertResult = priceLevel->value.AddOrder(order);

    OrderInfo entryInfo = OrderInfo{};
    entryInfo.priceLevel = &priceLevel->value;
    entryInfo.order = insertResult;

    orderLookup_.insert({order.orderId, entryInfo});

    return OrderResult::Success;
}

/// @brief Cancels a specific order from the orderbook by removing it from both
/// the price level and the order lookup table. Automatically removes the price
/// level if it's empty after the cancel
/// @param id Id of the order we want to cancel
/// @return OrderResult: an error code struct representing what happenned with
/// the order
OrderResult OrderBook::CancelOrder(const OrderId &id) {
    auto it = orderLookup_.find(id);
    if (it == orderLookup_.end())
        return OrderResult::OrderNotFound;

    // Delete FROM the price level
    PriceLevel *priceLevel = it->second.priceLevel;
    Side side = it->second.order->side;
    priceLevel->RemoveOrder(it->second.order);

    // Delete the price level if needed
    if (priceLevel->GetSize() <= 0) {
        Book &book = side == Side::Buy ? bids_ : asks_;
        Price priceKey =
            (side == Side::Buy) ? -priceLevel->price : priceLevel->price;
        bool deletionSuccess = book.delete_node(priceKey);
        if (!deletionSuccess) {
            return OrderResult::OrderNotFound;
        }
    }

    // If all went well, delete from the orderLookup_
    orderLookup_.erase(it);
    return OrderResult::Success;
}

/// @brief Modifies the order with a new Quantity less than the original,
/// maintaining price-time priority. If the newQty == 0, we automatically remove
/// from the Price level and order lookup table.
/// @param id id of the order
/// @param newQty new quantity
/// @return ModifyResult: an error code struct representing the outcome of the
/// operation
ModifyResult OrderBook::ModifyOrder(OrderId id, Quantity newQty) {
    // First, we find the order within the orderlookup
    auto it = orderLookup_.find(id);
    if (it == orderLookup_.end()) {
        return ModifyResult::OrderNotFound;
    }

    // Get the pointer to the actual order
    OrderInfo &entryInfo = it->second;
    PriceLevel *pl = entryInfo.priceLevel;
    OrderIterator order = entryInfo.order;

    if (newQty == 0) {
        auto res = CancelOrder(id);
        return (res == OrderResult::Success) ? ModifyResult::Success
                                             : ModifyResult::Rejected;
    }

    return pl->ModifyOrder(order, newQty);
}

const PriceLevel *OrderBook::bestAsk() const {
    auto *node = asks_.GetHead();
    if (!node) {
        return nullptr;
    }
    return &node->value;
}

const PriceLevel *OrderBook::bestBid() const {
    auto *bestBidNode = bids_.GetHead();
    if (!bestBidNode)
        return nullptr;
    return &bestBidNode->value;
}

/* ============================================================
   DISPLAY (L3, FORWARD ONLY)
   ============================================================ */
// This function is most likely unnecessary
bool isDarkMode() {
    const char *colorterm = std::getenv("COLORFG");
    if (colorterm && std::string(colorterm) == "light") {
        return false;
    }

    // Check if running in common dark mode terminals
    const char *term = std::getenv("TERM_PROGRAM");
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
    std::cout << "\n"
              << COLORS::bold << COLORS::cyan
              << "════════════════════════════════════════\n"
              << "         ORDER BOOK: " << symbol << "\n"
              << "════════════════════════════════════════" << COLORS::reset
              << "\n";

    // Display ASKS
    std::cout << "\n"
              << boxColor << askHeaderColor
              << "╔═══════════════════════════════════════╗\n"
              << "║            SELL SIDE (ASKS)           ║\n"
              << "╚═══════════════════════════════════════╝" << COLORS::reset
              << "\n\n";

    auto *askNode = asks_.GetHead();
    while (askNode) {
        PriceLevel &level = askNode->value;

        std::cout << priceColor << COLORS::bold << "Price: $" << level.price
                  << " (" << level.GetSize() << " orders)" << COLORS::reset
                  << "\n";

        for (const auto &order : level.orders) {
            std::cout << "  " << order << "\n";
        }
        std::cout << "\n";

        askNode = askNode->forward[0];
    }

    // Display BIDS
    std::cout << boxColor << bidHeaderColor
              << "╔═══════════════════════════════════════╗\n"
              << "║            BUY SIDE (BIDS)            ║\n"
              << "╚═══════════════════════════════════════╝" << COLORS::reset
              << "\n\n";

    auto *bidNode = bids_.GetHead();
    while (bidNode) {
        PriceLevel &level = bidNode->value;

        std::cout << priceColor << COLORS::bold << "Price: $" << level.price
                  << " (" << level.GetSize() << " orders)" << COLORS::reset
                  << "\n";

        for (const auto &order : level.orders) {
            std::cout << "  " << order << "\n";
        }
        std::cout << "\n";

        bidNode = bidNode->forward[0];
    }
}

void OrderBook::L2Snapshot() {
    bool darkMode = isDarkMode();

    std::string askColor = darkMode ? COLORS::magenta : COLORS::red;
    std::string bidColor = darkMode ? COLORS::green : COLORS::green;
    std::string priceColor = darkMode ? COLORS::yellow : COLORS::magenta;

    // UTF-8 block characters
    const std::string BLOCK = "█";
    const std::string LIGHT_BLOCK = "░";

    // Find max quantity for scaling
    Quantity maxQty = 0;

    auto *askNode = asks_.GetHead();
    while (askNode) {
        maxQty = std::max(maxQty, askNode->value.TotalQuantity());
        askNode = askNode->forward[0];
    }

    auto *bidNode = bids_.GetHead();
    while (bidNode) {
        maxQty = std::max(maxQty, bidNode->value.TotalQuantity());
        bidNode = bidNode->forward[0];
    }

    const int barWidth = 40;

    // Header
    std::cout << "\n"
              << COLORS::bold << COLORS::cyan << "         ╔════════════════ "
              << symbol << " L2 ════════════════╗" << COLORS::reset << "\n\n";

    // Asks - lowest to highest
    // std::cout << askColor << COLORS::bold << "ASKS:" << COLORS::reset <<
    // "\n";
    askNode = asks_.GetHead();
    while (askNode) {
        const PriceLevel &level = askNode->value;
        Quantity qty = level.TotalQuantity();
        int barLen = maxQty > 0 ? (qty * barWidth / maxQty) : 0;

        std::string bar;
        for (int i = 0; i < barLen; ++i)
            bar += BLOCK;
        for (int i = 0; i < barWidth - barLen; ++i)
            bar += LIGHT_BLOCK;

        std::cout << priceColor << std::setw(8) << level.price << COLORS::reset
                  << " │ " << askColor << bar << COLORS::reset << " " << qty
                  << "\n";

        askNode = askNode->forward[0];
    }

    // Spread
    const PriceLevel *bestAskLevel = bestAsk();
    const PriceLevel *bestBidLevel = bestBid();
    if (bestAskLevel && bestBidLevel) {
        Price spread = bestAskLevel->price - bestBidLevel->price;

        std::string line;
        for (int i = 0; i < barWidth; ++i) {
            line += "\xE2\x94\x80"; // UTF-8 encoding for ─
        }
        std::cout << COLORS::dim << "         ├" << line
                  << "┤ spread: " << spread << COLORS::reset << "\n";
    }

    // Bids - highest to lowest
    // std::cout << bidColor << COLORS::bold << "BIDS:" << COLORS::reset <<
    // "\n";
    bidNode = bids_.GetHead();
    while (bidNode) {
        const PriceLevel &level = bidNode->value;
        Quantity qty = level.TotalQuantity();
        int barLen = maxQty > 0 ? (qty * barWidth / maxQty) : 0;

        std::string bar;
        for (int i = 0; i < barLen; ++i)
            bar += BLOCK;
        for (int i = 0; i < barWidth - barLen; ++i)
            bar += LIGHT_BLOCK;

        std::cout << priceColor << std::setw(8) << level.price << COLORS::reset
                  << " │ " << bidColor << bar << COLORS::reset << " " << qty
                  << "\n";

        bidNode = bidNode->forward[0];
    }

    std::cout << "\n";
}

const OrderInfo *OrderBook::FindOrder(OrderId id) {
    auto it = orderLookup_.find(id);
    return (it == orderLookup_.end()) ? nullptr : &it->second;
}
