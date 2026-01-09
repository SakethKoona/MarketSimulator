#include "events.hpp"
#include "logger.hpp"
#include "nlohmann/json.hpp"
#include "orderbook.hpp"
#include <atomic>
#include <string>

using Symbol = std::string;

enum class EngineResult {
    Success,
    SymbolNotFound,
    OrderNotFound,
    Failed,
    NotEnoughLiquidity,
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
    EngineResult error_code;
};

struct SubmitResult {
    OrderId orderId;
    MatchResult matchRes;
};

class MatchingEngine {
  public:
    // public members
    std::string name;

    // Constructor
    MatchingEngine(EventSink &sink);

    // API's
    SubmitResult SubmitOrder(Symbol symbol, Price price, Quantity quantity,
                             Side side, OrderType type = OrderType::LIMIT,
                             TypeInForce tif = TypeInForce::GTC);
    EngineResult CancelOrder(OrderId id);
    EngineResult ModifyOrder(OrderId id, Quantity newQty,
                             std::optional<Price> newPrice = std::nullopt);
    void DisplayBook(Symbol symbol);
    void L2Snapshot(Symbol symbol);

  private:
    static std::atomic<OrderId> nextOrderId_;
    static std::atomic<TradeId> nextTradeId_;
    std::unordered_map<Symbol, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<OrderId, OrderBook *> orders_;

    MatchResult FillOrder(Order &order, OrderBook &book);
    SubmitResult SubmitOrderInternal(const Symbol &symbol, OrderId id,
                                     Price price, Quantity quantity, Side side,
                                     OrderType type, TypeInForce tif);
    static OrderId nextOrderId();
    static TradeId nextTradeId();
    Logger logger_;
    EventSink sink_;
};
