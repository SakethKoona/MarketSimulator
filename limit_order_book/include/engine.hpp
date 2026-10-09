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
    StatusCode DisplayBook(SymbolId symId);
    StatusCode L2Snapshot(SymbolId symId);
    void InitBooks(std::size_t numSymbols);

  private:
    // Per-engine sequences. The engine is single-threaded, so plain
    // integers; ids and sequences start at 1 (0 is the "no order" sentinel).
    TradeId nextTradeId_{1};
    uint64_t nextSeq_{1};
    std::vector<std::unique_ptr<OrderBook>> books_vec_;
    std::unordered_map<OrderId, SymbolId> orders_;

    // rest_action is what gets published if the order ends up resting:
    // Add for a new order, Replace for a cancel/replace keeping its id.
    FillResult FillOrder(Order &order, SymbolId symId,
                         BookAction rest_action = BookAction::Add);
    FillResult SubmitOrderInternal(SymbolId symId, OrderId id, Price price,
                                   Quantity quantity, Side side, OrderType type,
                                   TypeInForce tif,
                                   BookAction rest_action = BookAction::Add);
    TradeId nextTradeId();
    uint64_t nextSeq();
    bool CanFillAll(const Order &incoming, const OrderBook &book);
    void ReplaceOrder(OrderBook &book, OrderId id, Price price, Quantity qty,
                      Side side, OrderType type, TypeInForce tif);

    // Store references to event sink and sequencer, which are owned by the
    // exchange
    EventSink &sink_;
    Sequencer &sequencer_;
};
