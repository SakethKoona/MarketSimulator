#pragma once
// Turns the engine's OutBoundEvent stream into feed wire messages, one call
// per message. Owns the Execute + TradeFill pairing rule, so the publisher
// and the conformance tests share exactly one implementation.
#include "common.hpp"
#include "events.hpp"
#include "feed_encoder.hpp"
#include <cstdint>

class FeedFramer {
  public:
    explicit FeedFramer(EventSink &sink) : sink_(sink) {}

    // Writes the next wire message into out (>= feed::kMaxMessageSize) and
    // returns its length, or 0 when the sink is empty. When an Execute is
    // seen, waits up to wait_ns for its TradeFill partner (the engine emits
    // them back to back; the wait covers a preempted engine thread).
    std::uint16_t next(char *out, std::uint64_t wait_ns) {
        OutBoundEvent *ev = sink_.peek();
        if (!ev)
            return 0;

        if (auto *tf = std::get_if<TradeFillEvent>(ev)) {
            // A TradeFill without its Execute: volume is carried by Order
            // Executed, so there is nothing to publish.
            (void)tf;
            ++unpaired_;
            sink_.pop();
            return next(out, wait_ns);
        }

        const OrderBookEvent be = std::get<OrderBookEvent>(*ev);
        switch (be.action) {
        case BookAction::Add:
            sink_.pop();
            return feed::encode_add(be, out);
        case BookAction::Replace:
            sink_.pop();
            return feed::encode_replace(be, out);
        case BookAction::Modify:
            sink_.pop();
            return feed::encode_cancel(be, out);
        case BookAction::Delete:
            sink_.pop();
            return feed::encode_delete(be, out);
        case BookAction::Execute: {
            sink_.pop();
            const std::uint64_t deadline = get_monotonic_ns() + wait_ns;
            OutBoundEvent *nx = sink_.peek();
            while (!nx && get_monotonic_ns() < deadline)
                nx = sink_.peek();
            auto *tf = nx ? std::get_if<TradeFillEvent>(nx) : nullptr;
            if (tf && tf->book_seq == be.book_seq) {
                std::uint16_t n = feed::encode_executed(be, *tf, out);
                sink_.pop();
                return n;
            }
            // Partner missing: publish what we know so clients still shrink
            // or remove the resting order.
            ++unpaired_;
            TradeFillEvent none{};
            none.price = be.price;
            return feed::encode_executed(be, none, out);
        }
        }
        sink_.pop();
        return 0;
    }

    std::uint64_t unpaired() const { return unpaired_; }

  private:
    EventSink &sink_;
    std::uint64_t unpaired_ = 0;
};
