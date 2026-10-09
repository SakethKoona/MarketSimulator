#include "exchange.hpp"
#include <iostream>

int main() {
    std::cout << "╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║        MATCHING ENGINE - EVENT DEMO    ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    Exchange exchange;

    auto bid = exchange.SubmitOrder("AAPL", 100, 5, Side::Buy);
    exchange.SubmitOrder("AAPL", 105, 5, Side::Sell);
    exchange.SubmitOrder("AAPL", 103, 6, Side::Sell);
    exchange.SubmitOrder("AAPL", 103, 2, Side::Sell);

    // Cross the spread: buy 7 sweeps 6@103 fully and 1 of the 2@103
    exchange.SubmitOrder("AAPL", 104, 7, Side::Buy);

    // Shrink the resting bid, then move it up in price
    exchange.ModifyOrder(bid.order_id, 3);
    exchange.ModifyOrder(bid.order_id, 3, 101);
    exchange.CancelOrder(bid.order_id);

    exchange.L2Snapshot("AAPL");
    exchange.DisplayBook("AAPL");

    std::cout << "\n--- Event stream ---\n";
    exchange.DrainEvents(std::cout);

    return 0;
}
