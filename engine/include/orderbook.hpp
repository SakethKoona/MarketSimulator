#pragma once

#include "arena.hpp"
#include "common.hpp"
#include "errors.hpp"
#include "events.hpp"
#include "skiplist.hpp"
#include <chrono>
#include <cstdint>
#include <ctime>
#include <vector>

enum OrderType : uint8_t {
    MARKET,
    LIMIT,
};

enum TypeInForce : uint8_t {
    GTC, // Good-Till-Cancel
    FOK, // Fill-Or-Kill
    IOC, // Immediate-Or-Cancel
};

// Plain order data. 40 bytes: four 64-bit fields plus three byte enums.
struct Order {
    OrderId orderId;
    Price price;
    Quantity quantity;
    Timestamp timestamp;
    OrderType orderType;
    TypeInForce typeInForce;
    Side side;

    Order(OrderId orderId, Price price, Quantity quantity, OrderType orderType,
          TypeInForce typeInForce, Side side);
    Order(OrderId orderId, Price price, Quantity quantity, OrderType orderType,
          TypeInForce typeInForce, Side side, Timestamp timestamp);
};

struct PriceLevel;

// A resting order as stored in the book: the order plus intrusive links.
// Allocated from the book's arena pool; never individually heap-allocated.
struct OrderNode : Order {
    OrderNode *prev = nullptr;
    OrderNode *next = nullptr;
    PriceLevel *level = nullptr;
    OrderNode *free_next_ = nullptr; // ArenaPool free list

    explicit OrderNode(const Order &o) : Order(o) {}

    void Clear() {
        prev = next = nullptr;
        level = nullptr;
        free_next_ = nullptr;
    }
};

namespace COLORS {
constexpr const char *reset = "\033[0m";
constexpr const char *bold = "\033[1m";
constexpr const char *dim = "\033[2m";
constexpr const char *underline = "\033[4m";
constexpr const char *inverse = "\033[7m";

constexpr const char *red = "\033[31m";
constexpr const char *green = "\033[32m";
constexpr const char *yellow = "\033[33m";
constexpr const char *blue = "\033[34m";
constexpr const char *magenta = "\033[35m";
constexpr const char *cyan = "\033[36m";
} // namespace COLORS

constexpr int PRICE_W = 8;
constexpr int INDENT = 5;
constexpr int ORDER_W = 20;

void PrintOrderBookHeader(std::ostream &os);

std::ostream &operator<<(std::ostream &os, const Order &order);

// Forward iteration over the orders at a level, in time priority.
struct OrderRange {
    struct iterator {
        const OrderNode *node;
        const OrderNode &operator*() const { return *node; }
        const OrderNode *operator->() const { return node; }
        iterator &operator++() {
            node = node->next;
            return *this;
        }
        bool operator!=(const iterator &o) const { return node != o.node; }
    };
    const OrderNode *head;
    iterator begin() const { return {head}; }
    iterator end() const { return {nullptr}; }
};

// One price level: an intrusive FIFO of OrderNodes plus running totals.
// The book owns node allocation; the level only links and unlinks.
struct PriceLevel {
    Price price = 0;
    OrderNode *head = nullptr;
    OrderNode *tail = nullptr;
    Quantity totalQuantity = 0;
    int size_ = 0;

    void AddOrder(OrderNode *node);    // append, time priority
    void RemoveOrder(OrderNode *node); // unlink
    ModifyResult ModifyOrder(OrderNode *node, Quantity newQty);

    const OrderNode *front() const { return head; }
    const OrderNode *back() const { return tail; }
    OrderRange orders() const { return OrderRange{head}; }
    Quantity TotalQuantity() const { return totalQuantity; }
    int GetSize() const { return size_; }
    void SetPrice(Price p) { price = p; }
};

std::ostream &operator<<(std::ostream &os, const PriceLevel &pl);

using Book = SkipList<Price, PriceLevel>;

class OrderBook {
    Book bids_;
    Book asks_;

    // Order storage: pool-allocated nodes, looked up by dense OrderId.
    ArenaAllocator orderArena_;
    ArenaPool<OrderNode> orderPool_;
    std::vector<OrderNode *> orderLookup_; // indexed by OrderId; null = absent

    OrderBook(const OrderBook &) = delete;
    OrderBook &operator=(const OrderBook &) = delete;

  public:
    SymbolId symId;
    OrderBook();
    explicit OrderBook(SymbolId sym_id);
    ~OrderBook();

    const Book &bids() const;
    const Book &asks() const;
    OrderResult AddOrder(const Order &order);
    OrderResult CancelOrder(OrderId id);
    ModifyResult ModifyOrder(OrderId id, Quantity newQty);
    void Display();
    void L2Snapshot();

    const PriceLevel *BestBid() const;
    const PriceLevel *BestAsk() const;
    std::size_t size() const;        // number of price levels
    std::size_t orderCount() const;  // number of resting orders
    const OrderNode *FindOrder(OrderId id) const;

  private:
    // Indexed by seq_of(id): ids carry the shard in their high bits, and a
    // book only ever sees ids minted by its own shard.
    OrderNode *lookup(OrderId id) const {
        uint64_t seq = seq_of(id);
        return seq < orderLookup_.size() ? orderLookup_[seq] : nullptr;
    }
    void removeNode(OrderNode *node);
    std::size_t orderCount_ = 0;
};
