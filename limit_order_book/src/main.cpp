#include "exchange.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║   MATCHING ENGINE - MODIFY ORDER TESTS ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    Exchange exchange;

    exchange.SubmitOrder("AAPL", 100, 5, Side::Buy);
    exchange.SubmitOrder("AAPL", 100, 5, Side::Buy);

    exchange.SubmitOrder("AAPL", 95, 13, Side::Sell);
    exchange.L2Snapshot("AAPL");
    return 0;
}
