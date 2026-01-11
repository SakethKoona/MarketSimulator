// #include <iostream>
// #include <thread>
//
// void worker() {
//     // Sample worker thread that just prints someting out
//     std::cout << "Printing from Worker thread" << std::endl;
// }
//
// void worker2() {
//     std::cout << "Printing from the second worker thread" << std::endl;
// }
//
// int main() {
//     std::cout << "Event Emission Testing" << std::endl;
//     std::cout << "Part 1" << std::endl;
//
//     std::thread t(worker);
//     std::thread t2(worker2);
//     t.join();
//     t2.join();
//
//     std::cout << "printing from main" << std::endl;
// }
//

#include "engine.hpp"
#include <cassert>
#include <iostream>

void test_l2_snapshot() {
    std::cout << "\n=== TEST: L2 Snapshot ===" << std::endl;

    EventSink test_sink(1024 * 1024);
    MatchingEngine engine{test_sink};

    // Sell orders (asks)
    engine.SubmitOrder("AAPL", 115, 50, Side::Sell);
    engine.SubmitOrder("AAPL", 115, 30, Side::Sell); // Same level
    engine.SubmitOrder("AAPL", 112, 150, Side::Sell);
    engine.SubmitOrder("AAPL", 110, 250, Side::Sell);
    engine.SubmitOrder("AAPL", 108, 100, Side::Sell);
    engine.SubmitOrder("AAPL", 215, 50, Side::Sell);
    engine.SubmitOrder("AAPL", 215, 30, Side::Sell); // Same level
    engine.SubmitOrder("AAPL", 212, 150, Side::Sell);
    engine.SubmitOrder("AAPL", 210, 250, Side::Sell);
    engine.SubmitOrder("AAPL", 208, 100, Side::Sell);
    engine.SubmitOrder("AAPL", 279, 45, Side::Sell);

    // Buy orders (bids)
    engine.SubmitOrder("AAPL", 105, 500, Side::Buy);
    engine.SubmitOrder("AAPL", 105, 200, Side::Buy); // Same level
    engine.SubmitOrder("AAPL", 102, 200, Side::Buy);
    engine.SubmitOrder("AAPL", 100, 100, Side::Buy);
    engine.SubmitOrder("AAPL", 98, 75, Side::Buy);
    engine.SubmitOrder("AAPL", 95, 300, Side::Buy);
    engine.SubmitOrder("AAPL", 55, 500, Side::Buy);
    engine.SubmitOrder("AAPL", 55, 200, Side::Buy); // Same level
    engine.SubmitOrder("AAPL", 42, 200, Side::Buy);
    engine.SubmitOrder("AAPL", 40, 100, Side::Buy);
    engine.SubmitOrder("AAPL", 38, 75, Side::Buy);
    engine.SubmitOrder("AAPL", 25, 300, Side::Buy);

    std::cout << "\nFull order book display:" << std::endl;
    engine.DisplayBook("AAPL");

    std::cout << "\n\nL2 Snapshot (aggregated by price level):" << std::endl;
    engine.L2Snapshot("AAPL");
}

int main() {
    std::cout << "╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║   MATCHING ENGINE - MODIFY ORDER TESTS ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    test_l2_snapshot();
}
