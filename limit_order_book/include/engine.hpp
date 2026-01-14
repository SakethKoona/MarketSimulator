#pragma once

#include "events.hpp"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include "orderbook.hpp"
#include "types.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using Symbol = std::string;

enum class EngineResult {
    Success,
    SymbolNotFound,
    OrderNotFound,
    Failed,
    NotEnoughLiquidity,
};

struct Fill {
    OrderId orderId;
    Quantity qty;
    Price price;
    Timestamp time;
    Side side;
};

using TradeId = uint64_t;

struct Trade {
    TradeId id;
    Symbol symbol;
    Price price;
    Quantity quantity;

    Fill aggressor;
    Fill resting;
};

struct MatchResult {
    std::vector<Trade> trades;
    EngineResult error_code;
};

struct SubmitResult {
    OrderId orderId;
    MatchResult matchRes;
};

class MatchingEngine {
  public:
    // public members
    std::string name;

    // Constructor
    MatchingEngine(EventSink &sink);

    // API's
    SubmitResult SubmitOrder(SymbolId symId, Price price, Quantity quantity,
                             Side side, OrderType type = OrderType::LIMIT,
                             TypeInForce tif = TypeInForce::GTC);
    EngineResult CancelOrder(OrderId id);
    EngineResult ModifyOrder(OrderId id, Quantity newQty,
                             std::optional<Price> newPrice = std::nullopt);
    void DisplayBook(SymbolId symId);
    void L2Snapshot(Symbol symbol);
    void InitBooks(std::size_t numSymbols);

  private:
    static std::atomic<OrderId> nextOrderId_;
    static std::atomic<TradeId> nextTradeId_;
    std::unordered_map<Symbol, std::unique_ptr<OrderBook>> books_;
    std::vector<std::unique_ptr<OrderBook>> books_vec_;
    std::unordered_map<OrderId, OrderBook *> orders_;

    MatchResult FillOrder(Order &order, OrderBook &book);
    SubmitResult SubmitOrderInternal(SymbolId symId, OrderId id, Price price,
                                     Quantity quantity, Side side,
                                     OrderType type, TypeInForce tif);
    static OrderId nextOrderId();
    static TradeId nextTradeId();
    Logger logger_;
    EventSink sink_;
};
