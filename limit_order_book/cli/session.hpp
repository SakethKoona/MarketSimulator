#pragma once
// A CLI session is the thing commands act on. Two implementations: an
// in-process Exchange (shell / run) and a BOE connection to a running
// exchange_server (connect). Book inspection is only available in-process.
#include "common.hpp"
#include "exchange.hpp"
#include "results.hpp"
#include <functional>
#include <optional>
#include <string>
#include <vector>

struct OrderOutcome {
    StatusCode status = StatusCode::Success;
    FillStatus fill = FillStatus::Rejected;
    OrderId order_id = 0;
    Quantity executed = 0;
    Quantity remaining = 0;
    bool resting = false;
    std::string detail; // free text from a remote reject, etc.
};

struct Fill {
    OrderId order_id;
    Price price;
    Quantity qty;
    uint64_t match_id;
};

class Session {
  public:
    virtual ~Session() = default;

    virtual OrderOutcome Submit(const std::string &ticker, Side side,
                                Quantity qty, Price price, OrderType type,
                                TypeInForce tif) = 0;
    virtual StatusCode Cancel(OrderId id) = 0;
    virtual StatusCode Modify(OrderId id, Quantity qty,
                              std::optional<Price> price) = 0;

    // Fills observed since the last call (both sides for in-process, own
    // orders only for remote). Cleared by the call.
    virtual std::vector<Fill> TakeFills() = 0;

    // Book inspection; false if unsupported in this session type.
    virtual bool CanInspect() const = 0;
    virtual Exchange *exchange() { return nullptr; }

    virtual std::vector<std::string> Tickers() const = 0;
    virtual std::string Describe() const = 0;
};

// In-process session over an Exchange built from a config.
class LocalSession : public Session {
  public:
    explicit LocalSession(const json &cfg);

    OrderOutcome Submit(const std::string &ticker, Side side, Quantity qty,
                        Price price, OrderType type, TypeInForce tif) override;
    StatusCode Cancel(OrderId id) override;
    StatusCode Modify(OrderId id, Quantity qty,
                      std::optional<Price> price) override;
    std::vector<Fill> TakeFills() override;
    bool CanInspect() const override { return true; }
    Exchange *exchange() override { return &ex_; }
    std::vector<std::string> Tickers() const override;
    std::string Describe() const override;

    // Events drained from the sinks since the last TakeEvents(), newest last
    std::vector<OutBoundEvent> TakeEvents();

  private:
    void drain();
    Exchange ex_;
    std::vector<OutBoundEvent> events_;
    std::vector<Fill> fills_;
};
