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
#include "exchange.hpp"
#include <cassert>
#include <iostream>

int main() {
    std::cout << "╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║   MATCHING ENGINE - MODIFY ORDER TESTS ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;


    Exchange exchange;

    exchange.SubmitOrder("AAPL", 100, 5, Side::Buy, OrderType::LIMIT, TypeInForce::GTC);
    std::cout << "order submitted" << std::endl;
    exchange.L2Snapshot("AAPL");
    return 0;
}
