#include "engine.hpp"
#include <fstream>

using json = nlohmann::json;
std::atomic<OrderId> MatchingEngine::nextOrderId_{1};
std::atomic<TradeId> MatchingEngine::nextTradeId_{1};

bool IsPriceMoreAggressive(Price price, Price other, Side side) {
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
    json valid_symbols = data["symbols"];
    for (auto& [key, value] : valid_symbols.items()) {
        books_.emplace(key, OrderBook{});
    }
}

EngineResult MatchingEngine::SubmitOrder(Symbol symbol, Price price, Quantity quantity, Side side, OrderType type, TypeInForce tif) {

    auto it = books_.find(symbol);
    if (it == books_.end()) {
        return EngineResult::SymbolNotFound;
    }

    // If we do find the symbol, then we get its orderbook
    OrderBook ob = it->second;

    // Create and package the order
    Order order = Order(
        MatchingEngine::nextOrderId(), 
        price, 
        quantity, 
        type, 
        tif, 
        side
    );

    FillOrder(order, ob);

    return EngineResult::Success;
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


    while (incoming.quantity > 0) {
        const PriceLevel* price_level = (incoming.side == Side::Buy) ? book.bestAsk() : book.bestBid();

        if (!price_level) break; // Nothing to match, book was empty

        auto& resting = price_level->orders.front(); // Time priority, so we get fifo order
        Quantity adjustment = std::min(incoming.quantity, resting.quantity);
        Price exec_price = resting.price;

        if (incoming.orderType == OrderType::LIMIT && !IsPriceMoreAggressive(incoming.price, resting.price, resting.side)) break;

        incoming.quantity -= adjustment;
        book.ModifyOrder(resting.orderId, resting.quantity - adjustment);

        // Something has been matched, so we create a trade.
        Fill incoming_fill {
            .orderId = incoming.orderId,
            .price = exec_price,
            .qty = adjustment,
            .side = incoming.side,
            .time = incoming.timestamp
        };

        Fill resting_fill {
            .orderId = resting.orderId,
            .price = exec_price,
            .qty = adjustment,
            .side = resting.side,
            .time = resting.timestamp
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
}