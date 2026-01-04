#include "engine.hpp"
#include <fstream>

using json = nlohmann::json;
std::atomic<OrderId> MatchingEngine::nextOrderId_{1};
std::atomic<TradeId> MatchingEngine::nextTradeId_{1};

bool IsPriceMoreAggressive(Price price, Price other, Side side) {
    if (price == other) return true;

    if (side == Side::Buy) {
        return price > other;
    } else {
        return price < other;
    }
}

MatchingEngine::MatchingEngine() {
    // Reads everything from the configs
    std::ifstream file("../configs/default.json");
    json data = json::parse(file);

    // Go through all the symbols and create an orderbook for each one
    // Optional TODO: If we specify any orders already in the orderbook through the JSON config
    // We can load in the orderbooks with those orders specifically
    json valid_symbols = data["symbols"];
    for (auto& [stock, params] : valid_symbols.items()) {
        books_.emplace(stock, std::make_unique<OrderBook>("AAPL"));
    }
}

SubmitResult MatchingEngine::SubmitOrderInternal(Symbol symbol, OrderId id, Price price, Quantity quantity, Side side, OrderType type, TypeInForce tif) {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        throw std::runtime_error("Symbol not found");
    }

    // If we do find the symbol, then we get its orderbook
    OrderBook& ob = *it->second.get();

    // Create and package the order
    Order order = Order(
        id,
        price,
        quantity,
        type,
        tif,
        side
    );

    MatchResult res = FillOrder(order, ob);
    return { id, res };
}


SubmitResult MatchingEngine::SubmitOrder(Symbol symbol, Price price, Quantity quantity, Side side, OrderType type, TypeInForce tif) {
    OrderId id = MatchingEngine::nextOrderId();
    return SubmitOrderInternal(
        symbol,
        id,
        price,
        quantity,
        side,
        type,
        tif
    );
}

OrderId MatchingEngine::nextOrderId() {
    return nextOrderId_++;
}

TradeId MatchingEngine::nextTradeId() {
    return nextTradeId_++;
}

MatchResult MatchingEngine::FillOrder(Order& incoming, OrderBook& book) {
    /*
    Our matching logic:
    1. If the incoming is a MARKET incoming, we never want to add it to the book,
    so we keep filling at the best price level until we either fill completely
    or the orderbook is empty

    2. If the incoming is a LIMIT incoming, we want to fill at that price OR BETTER.
    */
    MatchResult res{};

    if (incoming.typeInForce == TypeInForce::FOK) {
        // std::cout << "We are in the FOK checking loop" << std::endl;
        Quantity remaining = incoming.quantity;
        bool canFillAll = false;
        const Book& match_book = (incoming.side == Side::Buy) ? book.asks() : book.bids();
        auto* level = match_book.GetHead();

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
            res.error_code = EngineResult::NotEnoughLiquidity;
            return res;
        }
    }

    while (incoming.quantity > 0) {
        const PriceLevel* price_level = (incoming.side == Side::Buy) ? book.bestAsk() : book.bestBid();

        if (price_level == nullptr) {
            // std::cout << "Book was empty, 0 price levels" << &incoming << std::endl;
            res.error_code = EngineResult::NotEnoughLiquidity;
            break;
        }; // Nothing to match, book was empty

        auto& resting = price_level->orders.front(); // Time priority, so we get fifo order
        Quantity adjustment = std::min(incoming.quantity, resting.quantity);
        Price exec_price = resting.price;

        // Early exit condition for limit orders
        if (incoming.orderType == OrderType::LIMIT && !IsPriceMoreAggressive(incoming.price, resting.price, incoming.side)) {
            break;
        }

        // Actual trade happenning in the order book
        Timestamp currentTime = std::chrono::duration_cast<std::chrono::nanoseconds>
            (std::chrono::high_resolution_clock::now().time_since_epoch())
            .count();

        Fill resting_fill {
            .orderId = resting.orderId,
            .price = exec_price,
            .qty = adjustment,
            .side = resting.side,
            .time = currentTime
        };

        incoming.quantity -= adjustment;
        if (adjustment == resting.quantity) {
            book.CancelOrder(resting.orderId);
        } else {
            book.ModifyOrder(resting.orderId, resting.quantity - adjustment);
        }

        // Something has been matched, so we create a trade.
        Fill incoming_fill {
            .orderId = incoming.orderId,
            .price = exec_price,
            .qty = adjustment,
            .side = incoming.side,
            .time = currentTime
        };

        Trade trade {
            .id = MatchingEngine::nextTradeId(),
            .symbol = book.symbol,
            .price = exec_price,
            .quantity = adjustment,
            .aggressor = incoming_fill,
            .resting = resting_fill
        };

        res.trades.push_back(trade);
    }

    switch (incoming.typeInForce)
    {
    case TypeInForce::GTC: // If we're good till cancel, then we check if we have any quantity, and we just add it to the price level
        if (incoming.quantity > 0 && incoming.orderType == OrderType::LIMIT) {
            book.addOrder(incoming);
            orders_.emplace(incoming.orderId, &book);
        }
        break;
    case TypeInForce::IOC: // Immediate or Cancel - Throw away the rest
        // Do nothing
        break;
    case TypeInForce::FOK: // How do we deal wit this? we either fill completely or throw this away completely
        // We deal with this earlier
        break;
    default:
        break;
    }

    res.error_code = EngineResult::Success;
    return res;
}

EngineResult MatchingEngine::CancelOrder(OrderId id) {
    // First, we search the order lookup in the engine to 1. get the engine, and also to check if the order was even processed
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return EngineResult::OrderNotFound;
    }

    // Otherwise, let's get the pointer to the book
    OrderBook& book = *it->second;


    // Then, we can just call cancel order
    auto result = book.CancelOrder(id);
    if (result == OrderResult::Success) {
        return EngineResult::Success;
    } else {
        return EngineResult::OrderNotFound;
    }
}

EngineResult MatchingEngine::ModifyOrder(OrderId id, Quantity newQty, std::optional<Price> newPrice) {
    auto it = orders_.find(id);
    if (it == orders_.end()) {
        return EngineResult::OrderNotFound;
    }

    // First, get the orderbook
    OrderBook& book = *it->second;
    const OrderInfo* resting = book.FindOrder(id);

    if (resting == nullptr) return EngineResult::OrderNotFound;

    // Then, there are two main cases
    // If only the quantity changed to something lower, then we can just call the orderbook's modify function
    if (newPrice || newQty > resting->order->quantity) {
        // We cancel and create
        Price oldPrice = resting->order->price;
        OrderId newId = resting->order->orderId;
        Side newSide = resting->order->side;
        OrderType newType = resting->order->orderType;
        TypeInForce newTif = resting->order->typeInForce;

        // book.CancelOrder(resting->order->orderId);
        // book.addOrder(newOrder);
        CancelOrder(resting->order->orderId);
        SubmitOrderInternal(
            book.symbol, 
            newId, 
            newPrice.value_or(oldPrice),
            newQty, 
            newSide, 
            newType, 
            newTif
        );
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

    OrderBook& ob = *it->second.get();
    ob.Display();
}

void MatchingEngine::L2Snapshot(Symbol symbol) {
    auto it = books_.find(symbol);
    if (it == books_.end()) {
        throw std::runtime_error("Couldn't find symbol in book");
    }

    OrderBook& ob = *it->second.get();
    ob.L2Snapshot();
}