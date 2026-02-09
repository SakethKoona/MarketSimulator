#pragma once

#include "common.hpp"
#include "events.hpp"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include "orderbook.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class StatusCode {
    Success,
    SymbolNotFound,
    OrderNotFound,
    Failed,
    NotEnoughLiquidity,
};

struct Fill {
    OrderId orderId;
    Quantity executed_qty;
    Price price;
    Timestamp time;
    Side side;
};

using TradeId = uint64_t;

struct Trade {
    TradeId id;
    SymbolId symId;
    Price price;
    Quantity quantity;

    Fill aggressor;
    Fill resting;
};

enum class FillResult {
    FullyFilled,
    PartiallyFilled,
    NotFilled,
};

struct MatchResult {
    std::vector<Trade> trades;
    StatusCode error_code;
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
    StatusCode CancelOrder(OrderId id);
    StatusCode ModifyOrder(OrderId id, Quantity newQty,
                           std::optional<Price> newPrice = std::nullopt);
    void DisplayBook(SymbolId symId);
    void L2Snapshot(SymbolId symId);
    void InitBooks(std::size_t numSymbols);

  private:
    static std::atomic<OrderId> nextOrderId_;
    static std::atomic<TradeId> nextTradeId_;
    std::vector<std::unique_ptr<OrderBook>> books_vec_;
    // std::unordered_map<OrderId, OrderBook *> orders_;

    std::unordered_map<OrderId, SymbolId> orders_;

    FillResult FillOrder(Order &order, SymbolId symId);
    SubmitResult SubmitOrderInternal(SymbolId symId, OrderId id, Price price,
                                     Quantity quantity, Side side,
                                     OrderType type, TypeInForce tif);
    static OrderId nextOrderId();
    static TradeId nextTradeId();
    bool CanFillAll(const Order &incoming, const OrderBook &book);
    Trade RunMatchingIteration(const Order &incoming, const Order &resting);
    Logger logger_;
    EventSink sink_;
};
