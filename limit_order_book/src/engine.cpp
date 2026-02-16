#include "engine.hpp"
#include "common.hpp"
#include "events.hpp"
#include "orderbook.hpp"
#include <cstddef>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
std::atomic<OrderId> MatchingEngine::nextOrderId_{1};
std::atomic<TradeId> MatchingEngine::nextTradeId_{1};

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
static std::string generateLogFilename() {
    auto logId = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
    const std::string &filename =
        std::to_string(logId) + "_MatchingEngineLog.txt";
    return filename;
}

MatchingEngine::MatchingEngine(EventSink &sink)
    : logger_(generateLogFilename()), sink_(sink) {}

void MatchingEngine::InitBooks(std::size_t numSymbols) {
    std::cout << "Init books has been called" << numSymbols << std::endl;
    books_vec_.reserve(numSymbols);
    for (std::size_t i = 0; i < numSymbols; i++) {
        books_vec_.emplace_back(std::make_unique<OrderBook>(i));
    }

    for (auto &el : books_vec_) {
        std::cout << "Element: " << el << std::endl;
    }
}

SubmitResult MatchingEngine::SubmitOrderInternal(SymbolId symId, OrderId id,
                                                 Price price, Quantity quantity,
                                                 Side side, OrderType type,
                                                 TypeInForce tif) {
    try {
        Order order = Order(id, price, quantity, type, tif, side);
        FillResult res = FillOrder(order, symId);
        // TODO: Fix this
        return {id, res};
    } catch (std::out_of_range) {
        throw std::runtime_error("Symbol not found from matching engine");
    }
}

SubmitResult MatchingEngine::SubmitOrder(SymbolId symId, Price price,
                                         Quantity quantity, Side side,
                                         OrderType type, TypeInForce tif) {
    OrderId id = MatchingEngine::nextOrderId();
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

FillResult MatchingEngine::FillOrder(Order &incoming, SymbolId symId) {
    OrderBook &book = *books_vec_[symId];

    // Initial FOK check -> O(n)
    if (incoming.typeInForce == TypeInForce::FOK &&
        !CanFillAll(incoming, book)) {
        return FillResult::NotFilled;
    }

    // Matching Logic
    while (incoming.quantity > 0) {
        const PriceLevel *price_level =
            (incoming.side == Side::Buy) ? book.bestAsk() : book.bestBid();

        if (price_level == nullptr) {
            return FillResult::PartiallyFilled;
        }

        // FIFO order
        auto &resting = price_level->orders.front();

        // Early exit condition for limit orders
        if (incoming.orderType == OrderType::LIMIT &&
            !IsPriceMoreAggressive(incoming.price, resting.price,
                                   incoming.side)) {
            break;
        }

        Quantity exec_quantity = std::min(incoming.quantity, resting.quantity);
        Price exec_price = resting.price;

        // Actual trade happenning in the order book
        Timestamp currentTime = get_current_timestamp();

        Fill resting_fill{.orderId = resting.orderId,
                          .executed_qty = exec_quantity,
                          .price = exec_price,
                          .time = currentTime,
                          .side = resting.side};

        incoming.quantity -= exec_quantity;
        if (exec_quantity == resting.quantity) {
            book.CancelOrder(resting.orderId);
        } else {
            book.ModifyOrder(resting.orderId, resting.quantity - exec_quantity);
        }

        // Something has been matched, so we create a trade.
        Fill incoming_fill{
            .orderId = incoming.orderId,
            .executed_qty = exec_quantity,
            .price = exec_price,
            .time = currentTime,
            .side = incoming.side,
        };

        Trade trade{.id = MatchingEngine::nextTradeId(),
                    .symId = book.symId,
                    .price = exec_price,
                    .quantity = exec_quantity,
                    .aggressor = incoming_fill,
                    .resting = resting_fill};
    }

    // For GTC partial fills, we add them to the book, for all other types,
    if (incoming.quantity > 0) {
        if (incoming.typeInForce == TypeInForce::GTC &&
            incoming.orderType == OrderType::LIMIT) {
            book.AddOrder(incoming);
            orders_.emplace(incoming.orderId, symId);
        }

        AddOrderEvent add_order{
            .sym_id = symId,
            .ref_number =, //.. generate from sequencer here todo,
            .price = incoming.price,
            .qty = incoming.quantity,
            .side = incoming.side,
        };

        Event emission{
            .type = EventType::OrderAdded,
            .timeGenerated = get_current_timestamp(),
            .add_event = add_order,
        };

        sink_.emit(emission);

        return FillResult::PartiallyFilled;
    }

    return FillResult::FullyFilled;
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

    // Then, we can just call cancel order
    auto result = book.CancelOrder(id);
    if (result == OrderResult::Success) {
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

    // First, get the orderbook
    const OrderInfo *resting = book.FindOrder(id);

    if (resting == nullptr)
        return StatusCode::OrderNotFound;

    // Case 1: changce price OR higher quantity
    if (newPrice || newQty > resting->order->quantity) {
        // We cancel and create
        Price oldPrice = resting->order->price;
        OrderId newId = resting->order->orderId;
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
