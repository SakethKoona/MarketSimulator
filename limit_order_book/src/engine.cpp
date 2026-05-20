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
std::atomic<OrderId> MatchingEngine::nextOrderId_{1};
std::atomic<TradeId> MatchingEngine::nextTradeId_{1};
std::atomic<uint64_t> MatchingEngine::nextSeq_{1};

FillResult::FillResult(OrderId id)
    : order_id(id), fill_status(FillStatus::Rejected),
      status_code(StatusCode::Success), qty_executed(0) {}

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
                                               TypeInForce tif) {
    // Avoid throwing for a missing symbol; return a rejected FillResult
    // instead.
    if (symId >= books_vec_.size()) {
        FillResult res(id);
        res.status_code = StatusCode::SymbolNotFound;
        res.fill_status = FillStatus::Rejected;
        return res;
    }

    Order order = Order(id, price, quantity, type, tif, side);
    FillResult res = FillOrder(order, symId);
    return res;
}

FillResult MatchingEngine::SubmitOrder(SymbolId symId, Price price,
                                       Quantity quantity, Side side,
                                       OrderType type, TypeInForce tif) {
    OrderId id = sequencer_.next(0);
    return SubmitOrderInternal(symId, id, price, quantity, side, type, tif);
}

OrderId MatchingEngine::nextOrderId() { return nextOrderId_++; }
TradeId MatchingEngine::nextTradeId() { return nextTradeId_++; }

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

FillResult MatchingEngine::FillOrder(Order &incoming, SymbolId sym_id) {
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

        if (exec_quantity == resting.quantity) {
            // If the minimum of the two is the resting, then we cancel the
            // resting and move on
            book.CancelOrder(resting.orderId);
        } else {
            // Otherwise, we Modify the qty of the remaining order, and the
            // incoming becomes 0
            book.ModifyOrder(resting.orderId, resting.orderId - exec_quantity);
        }
        res.qty_executed += exec_quantity;
        incoming.quantity -= exec_quantity;
    }

    // Handle GTC partial fills
    if (incoming.quantity > 0) {
        res.fill_status = FillStatus::PartiallyFilled;
        res.status_code = StatusCode::Success;
        res.qty_remaining = incoming.quantity;

        if (incoming.orderType == OrderType::LIMIT &&
            incoming.typeInForce == TypeInForce::GTC) {
            // Add it to the book
            book.AddOrder(incoming);
            res.qty_remaining = 0;
        }

        return res;
    }

    // Everythig has been filled
    res.status_code = StatusCode::Success;
    res.fill_status = FillStatus::FullyFilled;
    res.qty_remaining = 0;
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

    // Then, we can just call cancel order
    auto result = book.CancelOrder(id);
    if (result == OrderResult::Success) {
        // Emit the event

        sink_.emit_cancel_event(sym_id, id, MatchingEngine::nextSeq(),
                                cancelled->order->side,
                                cancelled->order->price);

        return StatusCode::Success;
    } else {
        return StatusCode::OrderNotFound;
    }
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

    // Case 1: changing price OR higher quantity
    if (newPrice || newQty > resting->order->quantity) {
        // We cancel and create
        Price oldPrice = resting->order->price;
        OrderId newId =
            resting->order->orderId; // New id stays the same as the old id
        Side newSide = resting->order->side;
        OrderType newType = resting->order->orderType;
        TypeInForce newTif = resting->order->typeInForce;

        // WARN: Dangerous if multithreaded, make this atomic
        CancelOrder(resting->order->orderId);
        SubmitOrderInternal(book.symId, newId, newPrice.value_or(oldPrice),
                            newQty, newSide, newType, newTif);
    } else if (newQty < resting->order->quantity) {
        book.ModifyOrder(resting->order->orderId, newQty);
    }

    return StatusCode::Success;
}

void MatchingEngine::DisplayBook(SymbolId symId) {
    try {
        auto &ob = books_vec_.at(symId);
        ob->Display();
    } catch (std::out_of_range) {
        throw std::runtime_error("Symbol Not Found");
    }
}

void MatchingEngine::L2Snapshot(SymbolId symId) {
    try {
        auto &ob = books_vec_.at(symId);
        ob->L2Snapshot();
    } catch (std::out_of_range) {
        throw std::runtime_error("Symbol Not Found");
    }
}
