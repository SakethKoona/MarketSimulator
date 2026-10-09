#include "orderbook.hpp"
#include "common.hpp"
#include <algorithm>
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
    : Order(orderId, price, quantity, orderType, typeInForce, side,
            get_current_timestamp()) {}

Order::Order(OrderId orderId, Price price, Quantity quantity,
             OrderType orderType, TypeInForce typeInForce, Side side,
             Timestamp timestamp)
    : orderId(orderId), price(price), quantity(quantity), timestamp(timestamp),
      orderType(orderType), typeInForce(typeInForce), side(side) {}

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

void PriceLevel::AddOrder(OrderNode *node) {
    node->level = this;
    node->next = nullptr;
    node->prev = tail;
    if (tail)
        tail->next = node;
    else
        head = node;
    tail = node;
    size_++;
    totalQuantity += node->quantity;
}

void PriceLevel::RemoveOrder(OrderNode *node) {
    if (node->prev)
        node->prev->next = node->next;
    else
        head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        tail = node->prev;
    node->prev = node->next = nullptr;
    node->level = nullptr;
    size_--;
    totalQuantity -= node->quantity;
}

ModifyResult PriceLevel::ModifyOrder(OrderNode *node, Quantity newQty) {
    if (newQty > node->quantity) {
        return ModifyResult::QtyIncreaseNotAllowed;
    }
    totalQuantity -= node->quantity - newQty;
    node->quantity = newQty;
    return ModifyResult::Success;
}

std::ostream &operator<<(std::ostream &os, const PriceLevel &pl) {
    os << COLORS::magenta << pl.price << COLORS::reset << std::endl;

    for (const auto &order : pl.orders()) {
        os << "  " << order << std::endl;
    }

    return os;
}

/* ============================================================
   ORDER BOOK
   ============================================================ */

// Order nodes are allocated from 1 MB blocks; the arena grows as needed.
static constexpr std::size_t kOrderArenaBlock = 1 << 20;

OrderBook::OrderBook() : OrderBook(static_cast<SymbolId>(-1)) {}
OrderBook::OrderBook(SymbolId sym_id)
    : bids_(0.5), asks_(0.5), orderArena_(kOrderArenaBlock),
      orderPool_(orderArena_), symId(sym_id) {}

OrderBook::~OrderBook() {
    // Nodes live in the arena, which frees everything at once; nothing to
    // walk. The skiplists free their own level nodes.
}

const Book &OrderBook::bids() const { return bids_; }
const Book &OrderBook::asks() const { return asks_; }
std::size_t OrderBook::size() const { return (bids_.len() + asks_.len()); }
std::size_t OrderBook::orderCount() const { return orderCount_; }

OrderResult OrderBook::AddOrder(const Order &order) {
    if (order.quantity == 0)
        return OrderResult::InvalidQty;

    if (lookup(order.orderId) != nullptr)
        return OrderResult::DuplicateOrder;

    auto &book = (order.side == Side::Buy) ? bids_ : asks_;
    Price priceKey = (order.side == Side::Buy) ? -order.price : order.price;

    auto *levelNode = book.insertOrGet(priceKey);
    PriceLevel &level = levelNode->value;
    level.SetPrice(order.price);

    OrderNode *node = orderPool_.allocate(order);
    level.AddOrder(node);

    uint64_t seq = seq_of(order.orderId);
    if (seq >= orderLookup_.size())
        orderLookup_.resize(
            std::max<std::size_t>(seq + 1, orderLookup_.size() * 2));
    orderLookup_[seq] = node;
    orderCount_++;

    return OrderResult::Success;
}

/// Unlinks a node from its level, drops the level if empty, releases the
/// node to the pool and clears the lookup slot.
void OrderBook::removeNode(OrderNode *node) {
    PriceLevel *level = node->level;
    Side side = node->side;
    OrderId id = node->orderId;

    level->RemoveOrder(node);

    if (level->GetSize() == 0) {
        Book &book = (side == Side::Buy) ? bids_ : asks_;
        Price priceKey = (side == Side::Buy) ? -level->price : level->price;
        book.delete_node(priceKey);
    }

    orderLookup_[seq_of(id)] = nullptr;
    orderPool_.deallocate(node);
    orderCount_--;
}

/// @brief Cancels an order: removes it from its price level and the lookup
/// table, and drops the price level if it is now empty.
OrderResult OrderBook::CancelOrder(OrderId id) {
    OrderNode *node = lookup(id);
    if (node == nullptr)
        return OrderResult::OrderNotFound;
    removeNode(node);
    return OrderResult::Success;
}

/// @brief Reduces an order's quantity in place, keeping price-time priority.
/// newQty == 0 cancels the order.
ModifyResult OrderBook::ModifyOrder(OrderId id, Quantity newQty) {
    OrderNode *node = lookup(id);
    if (node == nullptr)
        return ModifyResult::OrderNotFound;

    if (newQty == 0) {
        removeNode(node);
        return ModifyResult::Success;
    }

    return node->level->ModifyOrder(node, newQty);
}

const PriceLevel *OrderBook::BestAsk() const {
    auto *node = asks_.GetHead();
    return node ? &node->value : nullptr;
}

const PriceLevel *OrderBook::BestBid() const {
    auto *node = bids_.GetHead();
    return node ? &node->value : nullptr;
}

const OrderNode *OrderBook::FindOrder(OrderId id) const { return lookup(id); }

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
              << "         ORDER BOOK: " << symId << "\n"
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

        for (const auto &order : level.orders()) {
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

        for (const auto &order : level.orders()) {
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
    const PriceLevel *bestAskLevel = BestAsk();
    const PriceLevel *bestBidLevel = BestBid();
    if (bestAskLevel && bestBidLevel) {
        Price spread = bestAskLevel->price - bestBidLevel->price;

        std::string line;
        for (int i = 0; i < barWidth; ++i) {
            line += "\xE2\x94\x80"; // UTF-8 encoding for ─
        }
        std::cout << COLORS::dim << "         ├" << line
                  << "┤ spread: " << spread << COLORS::reset << "\n";
    }

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
