#include "engine.hpp"
#include "events.hpp"
#include <fstream>
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
    : logger_(generateLogFilename()), sink_(sink) {
    // Reads everything from the configs
    std::ifstream file("../configs/default.json");
    json data = json::parse(file);

    json valid_symbols = data["symbols"];
    for (auto &[stock, params] : valid_symbols.items()) {
        books_.emplace(stock, std::make_unique<OrderBook>(stock));
    }
}

SubmitResult MatchingEngine::SubmitOrderInternal(const Symbol &symbol,
                                                 OrderId id, Price price,
                                                 Quantity quantity, Side side,
                                                 OrderType type,
                                                 TypeInForce tif) {
    logger_.log(
        Level::INFO,
        symbol + "Order Submitted: " + (side == Side::Buy ? "BUY" : "SELL") +
            " " + std::to_string(quantity) + " @ " + std::to_string(price));

    auto it = books_.find(symbol);
    if (it == books_.end()) {
        throw std::runtime_error("Symbol not found");
    }

    // If we do find the symbol, then we get its orderbook
    OrderBook &ob = *it->second.get();

    // Create and package the order
    Order order = Order(id, price, quantity, type, tif, side);

    MatchResult res = FillOrder(order, ob);
    return {id, res};
}

SubmitResult MatchingEngine::SubmitOrder(Symbol symbol, Price price,
                                         Quantity quantity, Side side,
                                         OrderType type, TypeInForce tif) {
    OrderId id = MatchingEngine::nextOrderId();
    return SubmitOrderInternal(symbol, id, price, quantity, side, type, tif);
}

OrderId MatchingEngine::nextOrderId() { return nextOrderId_++; }

TradeId MatchingEngine::nextTradeId() { return nextTradeId_++; }

MatchResult MatchingEngine::FillOrder(Order &incoming, OrderBook &book) {
    /*
    Our matching logic:
    1. If the incoming is a MARKET incoming, we never want to add it to the
    book, so we keep filling at the best price level until we either fill
    completely or the orderbook is empty

    2. If the incoming is a LIMIT incoming, we want to fill at that price OR
    BETTER.
    */
    MatchResult res{};

    if (incoming.typeInForce == TypeInForce::FOK) {
        Quantity remaining = incoming.quantity;
        bool canFillAll = false;
        const Book &match_book =
            (incoming.side == Side::Buy) ? book.asks() : book.bids();
        auto *level = match_book.GetHead();

        /*
        We want to essentially go through all the price levels
        and simulate what it would be like to match without actually making
        any changes
        */
        while (level != nullptr) {
            Quantity resting = (*level).value.TotalQuantity();

            if (resting >= remaining) {
                canFillAll = true;
                break;
            } else { // Resting < remaining
                remaining -= resting;
            }

            level = level->Next(0);
        }

        if (!canFillAll) {
            logger_.log(Level::INFO,
                        "Order " + std::to_string(incoming.orderId) +
                            " failed FOK pre-screen, not enough liquidity.");
            res.error_code = EngineResult::NotEnoughLiquidity;
            return res;
        }
    }

    while (incoming.quantity > 0) {
        const PriceLevel *price_level =
            (incoming.side == Side::Buy) ? book.bestAsk() : book.bestBid();

        if (price_level == nullptr) {
            res.error_code = EngineResult::NotEnoughLiquidity;
            break;
        }; // Nothing to match, book was empty

        auto &resting =
            price_level->orders.front(); // Time priority, so we get fifo order

        // Early exit condition for limit orders
        if (incoming.orderType == OrderType::LIMIT &&
            !IsPriceMoreAggressive(incoming.price, resting.price,
                                   incoming.side)) {
            break;
        }

        Quantity adjustment = std::min(incoming.quantity, resting.quantity);
        Price exec_price = resting.price;

        // Actual trade happenning in the order book
        Timestamp currentTime =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now().time_since_epoch())
                .count();

        Fill resting_fill{.orderId = resting.orderId,
                          .qty = adjustment,
                          .price = exec_price,
                          .time = currentTime,
                          .side = resting.side};

        incoming.quantity -= adjustment;
        if (adjustment == resting.quantity) {
            book.CancelOrder(resting.orderId);
        } else {
            book.ModifyOrder(resting.orderId, resting.quantity - adjustment);
        }

        // Something has been matched, so we create a trade.
        Fill incoming_fill{
            .orderId = incoming.orderId,
            .qty = adjustment,
            .price = exec_price,
            .time = currentTime,
            .side = incoming.side,
        };

        Trade trade{.id = MatchingEngine::nextTradeId(),
                    .symbol = book.symbol,
                    .price = exec_price,
                    .quantity = adjustment,
                    .aggressor = incoming_fill,
                    .resting = resting_fill};

        res.trades.push_back(trade);
    }

    // For GTC partial fills, we add them to the book, for all other types,
    // we don't have to add anything yet.
    if (incoming.typeInForce == TypeInForce::GTC) {
        if (incoming.quantity > 0 && incoming.orderType == OrderType::LIMIT) {
            book.AddOrder(incoming);
            orders_.emplace(incoming.orderId, &book);
        }
    }

    res.error_code = EngineResult::Success;
    return res;
}

EngineResult MatchingEngine::CancelOrder(OrderId id) {
    // First, we search the order lookup in the engine to 1. get the engine,
    // and also to check if the order was even processed
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return EngineResult::OrderNotFound;
    }

    // Otherwise, let's get the pointer to the book
    OrderBook &book = *it->second;

    // Then, we can just call cancel order
    auto result = book.CancelOrder(id);
    if (result == OrderResult::Success) {
        return EngineResult::Success;
    } else {
        return EngineResult::OrderNotFound;
    }
}

EngineResult MatchingEngine::ModifyOrder(OrderId id, Quantity newQty,
                                         std::optional<Price> newPrice) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return EngineResult::OrderNotFound;
    }

    // First, get the orderbook
    OrderBook &book = *it->second;
    const OrderInfo *resting = book.FindOrder(id);

    if (resting == nullptr)
        return EngineResult::OrderNotFound;

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
        SubmitOrderInternal(book.symbol, newId, newPrice.value_or(oldPrice),
                            newQty, newSide, newType, newTif);
    } else if (newQty < resting->order->quantity) {
        book.ModifyOrder(resting->order->orderId, newQty);
    }

    return EngineResult::Success;
}

void MatchingEngine::DisplayBook(Symbol symbol) {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        throw std::runtime_error("Couldn't find symbol in book");
    }

    OrderBook &ob = *it->second.get();
    ob.Display();
}

void MatchingEngine::L2Snapshot(Symbol symbol) {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        throw std::runtime_error("Couldn't find symbol in book");
    }

    OrderBook &ob = *it->second.get();
    ob.L2Snapshot();
}
