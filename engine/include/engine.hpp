#pragma once

#include "common.hpp"
#include "events.hpp"
#include "nlohmann/json.hpp"
#include "orderbook.hpp"
#include "results.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// One matching engine = one shard. It owns the books for a subset of the
// symbols, mints OrderIds tagged with its shard index, and publishes to its
// own EventSink. It is single-threaded by contract: exactly one thread may
// call into a given engine. The Exchange routes between engines.
class MatchingEngine {
  public:
    // public members
    std::string name;

    MatchingEngine(EventSink &sink, ShardId shard = 0);

    // API's
    FillResult SubmitOrder(SymbolId symId, Price price, Quantity quantity,
                           Side side, OrderType type = OrderType::LIMIT,
                           TypeInForce tif = TypeInForce::GTC);
    StatusCode CancelOrder(OrderId id);
    StatusCode ModifyOrder(OrderId id, Quantity newQty,
                           std::optional<Price> newPrice = std::nullopt);
    StatusCode DisplayBook(SymbolId symId);
    StatusCode L2Snapshot(SymbolId symId);

    // Creates books for every symbol id in [0, numSymbols) (single shard).
    void InitBooks(std::size_t numSymbols);
    // Creates books only for the given symbols; other ids are not served.
    void InitBooks(const std::vector<SymbolId> &symbols,
                   std::size_t numSymbols);

    ShardId Shard() const { return shard_; }
    // Symbol of a resting order on this shard, if any
    std::optional<SymbolId> SymbolOfOrder(OrderId id) const {
        SymbolId s = symbolOf(id);
        return s == kNoSymbol ? std::nullopt : std::optional<SymbolId>(s);
    }
    bool Serves(SymbolId symId) const {
        return symId < books_vec_.size() && books_vec_[symId] != nullptr;
    }

    // Read-only view of a book, for conformance tests and snapshots.
    const OrderBook &GetBook(SymbolId symId) const { return *books_vec_.at(symId); }

  private:
    ShardId shard_;

    // Per-engine sequences, each tagged with the shard in the top 8 bits so
    // order ids, trade ids and book_seq are globally unique with no shared
    // counter. The engine is single-threaded, so plain integers; sequences
    // start at 1 (0 is the "no id" sentinel).
    uint64_t nextOrderSeq_{1};
    uint64_t nextTradeSeq_{1};
    uint64_t nextBookSeq_{1};
    std::vector<std::unique_ptr<OrderBook>> books_vec_;

    // seq_of(OrderId) -> SymbolId for every resting order on this shard.
    static constexpr SymbolId kNoSymbol = static_cast<SymbolId>(-1);
    std::vector<SymbolId> orders_;
    SymbolId symbolOf(OrderId id) const {
        if (shard_of(id) != shard_)
            return kNoSymbol;
        uint64_t seq = seq_of(id);
        return seq < orders_.size() ? orders_[seq] : kNoSymbol;
    }
    void setSymbol(OrderId id, SymbolId sym) {
        uint64_t seq = seq_of(id);
        if (seq >= orders_.size())
            orders_.resize(std::max<std::size_t>(seq + 1, orders_.size() * 2),
                           kNoSymbol);
        orders_[seq] = sym;
    }
    void clearSymbol(OrderId id) {
        uint64_t seq = seq_of(id);
        if (seq < orders_.size())
            orders_[seq] = kNoSymbol;
    }

    // rest_action is what gets published if the order ends up resting:
    // Add for a new order, Replace for a cancel/replace keeping its id.
    FillResult FillOrder(Order &order, SymbolId symId,
                         BookAction rest_action = BookAction::Add);
    FillResult SubmitOrderInternal(SymbolId symId, OrderId id, Price price,
                                   Quantity quantity, Side side, OrderType type,
                                   TypeInForce tif,
                                   BookAction rest_action = BookAction::Add);
    OrderId nextOrderId() { return make_order_id(shard_, nextOrderSeq_++); }
    TradeId nextTradeId() { return make_shard_id(shard_, nextTradeSeq_++); }
    uint64_t nextSeq() { return make_shard_id(shard_, nextBookSeq_++); }
    bool CanFillAll(const Order &incoming, const OrderBook &book);
    void ReplaceOrder(OrderBook &book, OrderId id, Price price, Quantity qty,
                      Side side, OrderType type, TypeInForce tif);

    // Event sink owned by the exchange, one per shard
    EventSink &sink_;
};
