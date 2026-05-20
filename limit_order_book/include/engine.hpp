#pragma once

#include "common.hpp"
#include "events.hpp"
#include "nlohmann/json.hpp"
#include "orderbook.hpp"
#include "results.hpp"
#include "sequencer.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MatchingEngine {
  public:
    // public members
    std::string name;

    // Constructor
    MatchingEngine(EventSink &sink, Sequencer &seq);

    // API's
    FillResult SubmitOrder(SymbolId symId, Price price, Quantity quantity,
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
    static std::atomic<uint64_t> nextSeq_;
    std::vector<std::unique_ptr<OrderBook>> books_vec_;
    std::unordered_map<OrderId, SymbolId> orders_;

    FillResult FillOrder(Order &order, SymbolId symId);
    FillResult SubmitOrderInternal(SymbolId symId, OrderId id, Price price,
                                   Quantity quantity, Side side, OrderType type,
                                   TypeInForce tif);
    static OrderId nextOrderId();
    static TradeId nextTradeId();
    static uint64_t nextSeq();
    bool CanFillAll(const Order &incoming, const OrderBook &book);

    // Store references to event sink and sequencer, which are owned by the
    // exchange
    const EventSink &sink_;
    const Sequencer &sequencer_;
};
