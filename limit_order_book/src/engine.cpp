#include "engine.hpp"
#include "common.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include "results.hpp"
#include "sequencer.hpp"
#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
FillResult::FillResult(OrderId id)
    : order_id(id), fill_status(FillStatus::Rejected),
      status_code(StatusCode::Success), qty_executed(0), qty_remaining(0),
      resting(false) {}

bool IsPriceMoreAggressive(Price price, Price other, Side side) {
    if (price == other)
        return true;

    if (side == Side::Buy) {
        return price > other;
    } else {
        return price < other;
    }
}

// Helper function to init the logger
// static std::string generateLogFilename() {
//     auto logId = std::chrono::duration_cast<std::chrono::seconds>(
//                      std::chrono::steady_clock::now().time_since_epoch())
//                      .count();
//     const std::string &filename =
//         std::to_string(logId) + "_MatchingEngineLog.txt";
//     return filename;
// }

MatchingEngine::MatchingEngine(EventSink &sink, Sequencer &seq)
    : sink_(sink), sequencer_(seq) {}

void MatchingEngine::InitBooks(std::size_t numSymbols) {
    books_vec_.reserve(numSymbols);
    for (std::size_t i = 0; i < numSymbols; i++) {
        books_vec_.emplace_back(std::make_unique<OrderBook>(i));
    }
}

FillResult MatchingEngine::SubmitOrderInternal(SymbolId symId, OrderId id,
                                               Price price, Quantity quantity,
                                               Side side, OrderType type,
                                               TypeInForce tif,
                                               BookAction rest_action) {
    // Avoid throwing for a missing symbol; return a rejected FillResult
    // instead.
    if (symId >= books_vec_.size()) {
        FillResult res(id);
        res.status_code = StatusCode::SymbolNotFound;
        res.fill_status = FillStatus::Rejected;
        return res;
    }
    if (quantity == 0) {
        FillResult res(id);
        res.status_code = StatusCode::InvalidQuantity;
        res.fill_status = FillStatus::Rejected;
        return res;
    }
    if (type == OrderType::LIMIT && price == 0) {
        FillResult res(id);
        res.status_code = StatusCode::InvalidPrice;
        res.fill_status = FillStatus::Rejected;
        return res;
    }

    Order order = Order(id, price, quantity, type, tif, side);
    return FillOrder(order, symId, rest_action);
}

FillResult MatchingEngine::SubmitOrder(SymbolId symId, Price price,
                                       Quantity quantity, Side side,
                                       OrderType type, TypeInForce tif) {
    OrderId id = sequencer_.next(0);
    return SubmitOrderInternal(symId, id, price, quantity, side, type, tif);
}

TradeId MatchingEngine::nextTradeId() { return nextTradeId_++; }
uint64_t MatchingEngine::nextSeq() { return nextSeq_++; }

bool MatchingEngine::CanFillAll(const Order &incoming, const OrderBook &book) {
    Quantity remaining = incoming.quantity;

    const Book &match_book =
        (incoming.side == Side::Buy) ? book.asks() : book.bids();

    auto *level = match_book.GetHead();

    // Keep potentially matching until we can't anymore or we fill completely
    while (level != nullptr) {

        // Check for Limit orders
        if (incoming.orderType == OrderType::LIMIT &&
            !IsPriceMoreAggressive(incoming.price, level->value.price,
                                   incoming.side)) {
            return false;
        }

        Quantity resting_qty = (*level).value.TotalQuantity();

        if (resting_qty >= remaining)
            return true;

        remaining -= resting_qty;
        level = level->Next(0);
    }

    return false;
}

FillResult MatchingEngine::FillOrder(Order &incoming, SymbolId sym_id,
                                     BookAction rest_action) {
    // First, get the orderbook
    OrderBook &book = *books_vec_.at(sym_id);
    FillResult res = FillResult(incoming.orderId);
    res.qty_remaining = incoming.quantity;

    // Next, we make the FOK Check and terminate early if needed
    if (incoming.typeInForce == TypeInForce::FOK &&
        !CanFillAll(incoming, book)) {
        // Terminate and return the result
        res.status_code = StatusCode::FOKFailed;
        res.qty_executed = 0;
        res.fill_status = FillStatus::Rejected;
        if (rest_action == BookAction::Replace) {
            sink_.emit_cancel_event(sym_id, incoming.orderId, nextSeq(),
                                    incoming.side, incoming.price);
        }
        return res;
    }

    // Matching logic loop
    while (incoming.quantity > 0) {
        // 1. Find the relevant price level, depending on the side of the
        // incoming
        const PriceLevel *priceLevel =
            (incoming.side == Side::Buy) ? book.BestAsk() : book.BestBid();

        // 2. If the price level is empty, we stop the loop
        if (priceLevel == nullptr) {
            res.status_code = StatusCode::Success;
            res.fill_status = (res.qty_executed == 0)
                                  ? FillStatus::Rejected
                                  : FillStatus::PartiallyFilled;
            break;
        }

        // 3. Otherwise, we get the first order from that price level
        const Order resting = priceLevel->orders.front();

        if (incoming.orderType == OrderType::LIMIT &&
            !IsPriceMoreAggressive(incoming.price, resting.price,
                                   incoming.side)) {
            break;
        }

        Quantity exec_quantity = std::min(incoming.quantity, resting.quantity);
        Quantity resting_remaining = resting.quantity - exec_quantity;

        // One book_seq covers both perspectives of this match
        uint64_t seq = nextSeq();

        if (resting_remaining == 0) {
            // Resting order fully consumed: remove it from the book
            book.CancelOrder(resting.orderId);
            orders_.erase(resting.orderId);
        } else {
            // Resting order partially consumed: shrink it in place
            book.ModifyOrder(resting.orderId, resting_remaining);
        }

        // Every match is exactly one Execute for the resting side paired
        // with one TradeFill on the same book_seq. Execute with
        // remaining 0 means the order left the book; no Delete follows.
        // Delete is reserved for owner cancels and replaces that rest
        // nothing.
        sink_.emit_execute_event(sym_id, resting.orderId, seq, resting.side,
                                 resting.price, resting_remaining);

        // Trade always executes at the resting price
        sink_.emit_trade_event(sym_id, nextTradeId(), incoming.orderId,
                               resting.orderId, incoming.side, resting.price,
                               exec_quantity, seq);

        res.qty_executed += exec_quantity;
        incoming.quantity -= exec_quantity;
    }

    // Handle unfilled remainder
    if (incoming.quantity > 0) {
        res.status_code = StatusCode::Success;
        res.qty_remaining = incoming.quantity;

        if (incoming.orderType == OrderType::LIMIT &&
            incoming.typeInForce == TypeInForce::GTC) {
            // Rest it on the book and remember which book it lives in
            OrderResult added = book.AddOrder(incoming);
            if (added != OrderResult::Success) {
                res.fill_status = FillStatus::Rejected;
                res.status_code = (added == OrderResult::DuplicateOrder)
                                      ? StatusCode::DuplicateOrder
                                      : StatusCode::Failed;
                return res;
            }
            orders_[incoming.orderId] = sym_id;
            sink_.emit_book_event(rest_action, sym_id, incoming.orderId,
                                  nextSeq(), incoming.side, incoming.price,
                                  incoming.quantity);
            res.resting = true;
            res.fill_status = (res.qty_executed == 0)
                                  ? FillStatus::Accepted
                                  : FillStatus::PartiallyFilled;
            return res;
        }

        // Did not rest (IOC / market with no more liquidity)
        res.fill_status = (res.qty_executed == 0) ? FillStatus::Rejected
                                                  : FillStatus::PartiallyFilled;
        if (rest_action == BookAction::Replace) {
            // IOC/FOK/market replacement that did not rest: the old order
            // is gone and nothing took its place
            sink_.emit_cancel_event(sym_id, incoming.orderId, nextSeq(),
                                    incoming.side, incoming.price);
        }

        return res;
    }

    // Everything has been filled
    res.status_code = StatusCode::Success;
    res.fill_status = FillStatus::FullyFilled;
    res.qty_remaining = 0;
    if (rest_action == BookAction::Replace) {
        // Replacement was fully matched; close out the old resting order
        sink_.emit_cancel_event(sym_id, incoming.orderId, nextSeq(),
                                incoming.side, incoming.price);
    }
    return res;
}

StatusCode MatchingEngine::CancelOrder(OrderId id) {
    // First, we search the order lookup in the engine to 1. get the engine,
    // and also to check if the order was even processed
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return StatusCode::OrderNotFound;
    }

    // Otherwise, let's get the pointer to the book
    SymbolId sym_id = it->second;
    OrderBook &book = *books_vec_[sym_id];

    const OrderInfo *cancelled = book.FindOrder(id);
    if (cancelled == nullptr) {
        return StatusCode::OrderNotFound;
    }

    // Copy what we need before the book erases the order
    Side side = cancelled->order->side;
    Price price = cancelled->order->price;

    auto result = book.CancelOrder(id);
    if (result == OrderResult::Success) {
        orders_.erase(it);
        sink_.emit_cancel_event(sym_id, id, MatchingEngine::nextSeq(), side,
                                price);
        return StatusCode::Success;
    } else {
        return StatusCode::OrderNotFound;
    }
}

void MatchingEngine::ReplaceOrder(OrderBook &book, OrderId id, Price price,
                                  Quantity qty, Side side, OrderType type,
                                  TypeInForce tif) {
    SymbolId sym_id = book.symId;

    // Remove the old order without publishing a Delete. FillOrder publishes
    // the outcome: any trades, then a Replace carrying the new resting state,
    // or a Delete if nothing is left to rest.
    book.CancelOrder(id);
    orders_.erase(id);
    SubmitOrderInternal(sym_id, id, price, qty, side, type, tif,
                        BookAction::Replace);
}

StatusCode MatchingEngine::ModifyOrder(OrderId id, Quantity newQty,
                                       std::optional<Price> newPrice) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return StatusCode::OrderNotFound;
    }

    SymbolId sym_id = it->second;
    OrderBook &book = *books_vec_[sym_id];

    // First, get the order
    const OrderInfo *resting = book.FindOrder(id);

    if (resting == nullptr)
        return StatusCode::OrderNotFound;

    // Case 0: qty 0 is a cancel
    if (newQty == 0) {
        return CancelOrder(id);
    }

    // Case 1: changing price OR higher quantity
    if (newPrice || newQty > resting->order->quantity) {
        // We cancel and create
        Price oldPrice = resting->order->price;
        OrderId newId =
            resting->order->orderId; // New id stays the same as the old id
        Side newSide = resting->order->side;
        OrderType newType = resting->order->orderType;
        TypeInForce newTif = resting->order->typeInForce;

        Price replacePrice = newPrice.value_or(oldPrice);

        // WARN: Dangerous if multithreaded, make this atomic
        // `resting` is invalid after this call; use the copies above.
        // Replace is published as a single event rather than a Delete
        // followed by an Add, so downstream clients keep the order's
        // identity. Any matches the replacement triggers are emitted by
        // FillOrder as usual.
        ReplaceOrder(book, newId, replacePrice, newQty, newSide, newType,
                     newTif);
    } else if (newQty < resting->order->quantity) {
        Side side = resting->order->side;
        Price price = resting->order->price;
        if (book.ModifyOrder(id, newQty) == ModifyResult::Success) {
            sink_.emit_modify_event(sym_id, id, nextSeq(), side, price,
                                    newQty);
        }
    }

    return StatusCode::Success;
}

StatusCode MatchingEngine::DisplayBook(SymbolId symId) {
    if (symId >= books_vec_.size())
        return StatusCode::SymbolNotFound;
    books_vec_[symId]->Display();
    return StatusCode::Success;
}

StatusCode MatchingEngine::L2Snapshot(SymbolId symId) {
    if (symId >= books_vec_.size())
        return StatusCode::SymbolNotFound;
    books_vec_[symId]->L2Snapshot();
    return StatusCode::Success;
}
