#include "../include/engine.hpp"
#include <iostream>
#include <cassert>

void test_modify_order_quantity() {
    std::cout << "\n=== TEST: Modify Order Quantity ===" << std::endl;
    
    MatchingEngine engine{};
    
    // Submit a buy order
    auto result = engine.SubmitOrder("AAPL", 100, 50, Side::Buy);
    OrderId orderId = result.orderId;
    
    std::cout << "Submitted order " << orderId << " - Buy 50 @ $100" << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify quantity down
    auto modifyResult = engine.ModifyOrder(orderId, 30);
    assert(modifyResult == EngineResult::Success);
    
    std::cout << "\nModified to 30 shares" << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify quantity up
    modifyResult = engine.ModifyOrder(orderId, 75);
    assert(modifyResult == EngineResult::Success);
    
    std::cout << "\nModified to 75 shares" << std::endl;
    engine.DisplayBook("AAPL");
}

void test_modify_order_price() {
    std::cout << "\n=== TEST: Modify Order Price ===" << std::endl;
    
    MatchingEngine engine{};
    
    // Submit orders at different prices
    auto r1 = engine.SubmitOrder("AAPL", 100, 10, Side::Buy);
    auto r2 = engine.SubmitOrder("AAPL", 105, 20, Side::Buy);
    auto r3 = engine.SubmitOrder("AAPL", 110, 15, Side::Sell);
    
    std::cout << "Initial book:" << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify price of first buy order
    auto modifyResult = engine.ModifyOrder(r1.orderId, 10, 103);
    assert(modifyResult == EngineResult::Success);
    
    std::cout << "\nModified order " << r1.orderId << " to price $103" << std::endl;
    engine.DisplayBook("AAPL");
}

void test_modify_order_price_and_quantity() {
    std::cout << "\n=== TEST: Modify Order Price and Quantity ===" << std::endl;
    
    MatchingEngine engine{};
    
    auto result = engine.SubmitOrder("AAPL", 100, 50, Side::Buy);
    OrderId orderId = result.orderId;
    
    std::cout << "Submitted order " << orderId << " - Buy 50 @ $100" << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify both price and quantity
    auto modifyResult = engine.ModifyOrder(orderId, 75, 105);
    assert(modifyResult == EngineResult::Success);
    
    std::cout << "\nModified to Buy 75 @ $105" << std::endl;
    engine.DisplayBook("AAPL");
}

void test_modify_triggers_match() {
    std::cout << "\n=== TEST: Modify Triggers Match ===" << std::endl;
    
    MatchingEngine engine{};
    
    // Place resting sell order
    auto sellResult = engine.SubmitOrder("AAPL", 110, 20, Side::Sell);
    
    // Place buy order below
    auto buyResult = engine.SubmitOrder("AAPL", 100, 15, Side::Buy);
    
    std::cout << "Initial book:" << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify buy price up to cross the spread - should trigger match
    std::cout << "\nModifying buy order to $110 (should match)..." << std::endl;
    engine.ModifyOrder(buyResult.orderId, 15, 110);
    
    std::cout << "\nAfter modification:" << std::endl;
    engine.DisplayBook("AAPL");
}

void test_modify_nonexistent_order() {
    std::cout << "\n=== TEST: Modify Non-existent Order ===" << std::endl;
    
    MatchingEngine engine{};
    
    auto result = engine.ModifyOrder(99999, 100, 100);
    assert(result == EngineResult::OrderNotFound);
    
    std::cout << "Correctly returned OrderNotFound for invalid order ID" << std::endl;
}

void test_modify_to_zero_quantity() {
    std::cout << "\n=== TEST: Modify to Zero Quantity ===" << std::endl;
    
    MatchingEngine engine{};
    
    auto result = engine.SubmitOrder("AAPL", 100, 50, Side::Buy);
    OrderId orderId = result.orderId;
    
    std::cout << "Submitted order " << orderId << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify to 0 should cancel the order
    std::cout << "\nModifying to 0 quantity (should cancel)..." << std::endl;
    auto modifyResult = engine.ModifyOrder(orderId, 0);
    
    engine.DisplayBook("AAPL");
    
    // Try to modify again - should fail
    modifyResult = engine.ModifyOrder(orderId, 10);
    assert(modifyResult == EngineResult::OrderNotFound);
    std::cout << "Order correctly removed from book" << std::endl;
}

void test_modify_partial_fill_then_modify() {
    std::cout << "\n=== TEST: Modify After Partial Fill ===" << std::endl;
    
    MatchingEngine engine{};
    
    // Submit buy order
    auto buyResult = engine.SubmitOrder("AAPL", 100, 50, Side::Buy);
    
    // Partially fill it
    auto sellResult = engine.SubmitOrder("AAPL", 100, 20, Side::Sell);
    
    std::cout << "After partial fill (30 remaining):" << std::endl;
    engine.DisplayBook("AAPL");
    
    // Modify the remaining quantity
    std::cout << "\nModifying remaining quantity to 40..." << std::endl;
    auto modifyResult = engine.ModifyOrder(buyResult.orderId, 40);
    assert(modifyResult == EngineResult::Success);
    
    engine.DisplayBook("AAPL");
}

int main() {
    std::cout << "╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║   MATCHING ENGINE - MODIFY ORDER TESTS ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    test_modify_order_quantity();
    test_modify_order_price();
    test_modify_order_price_and_quantity();
    test_modify_triggers_match();
    test_modify_nonexistent_order();
    test_modify_to_zero_quantity();
    test_modify_partial_fill_then_modify();

    std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
    std::cout << "║         ALL TESTS COMPLETED!           ║" << std::endl;
    std::cout << "╚════════════════════════════════════════╝" << std::endl;

    return 0;
}
