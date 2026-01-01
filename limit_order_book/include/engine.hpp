#include "orderbook.hpp"
#include "nlohmann/json.hpp"
#include <atomic>

using Symbol = std::string;

enum class EngineResult {
    Success,
    SymbolNotFound,
    OrderNotFound,
    Failed
};


struct Fill {
    OrderId orderId;
    Quantity qty;
    Price price;
    Timestamp time;
    Side side;
};

using TradeId = uint64_t;

struct Trade {
    TradeId id;
    Symbol symbol;
    Price price;
    Quantity quantity;

    Fill aggressor;
    Fill resting;
};


struct MatchResult {
    std::vector<Trade> trades;
};

class MatchingEngine {
public:
    // Constructor
    MatchingEngine();

    // API's
    EngineResult SubmitOrder(Symbol symbol, Price price, Quantity quantity, Side side, OrderType type = OrderType::LIMIT, TypeInForce tif = TypeInForce::GTC);
    EngineResult CancelOrder(OrderId id);
    EngineResult ModifyOrder(OrderId id, Quantity newQty, std::optional<Price> newPrice = std::nullopt);


private:
    static std::atomic<OrderId> nextOrderId_;
    static std::atomic<TradeId> nextTradeId_;

    std::unordered_map<Symbol, OrderBook> books_;
    std::unordered_map<OrderId, OrderBook*> orders_;

    MatchResult FillOrder(Order& order, OrderBook& book);
    static OrderId nextOrderId();
    static TradeId nextTradeId();
};
