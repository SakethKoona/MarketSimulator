# MarketSimulator

**MarketSimulator** is an in-progress **C++ market exchange engine** with multi-symbol Level 3 orderbooks, designed for performance and realism. It’s a foundation for simulating market dynamics and experimenting with advanced order flow models.

---

## Features (Implemented)

- **Multi-Symbol Support:** Each symbol has its own independent orderbook.  
- **Level 3 Orderbook:** Tracks every individual order at each price level.  
- **Skiplist for Price Levels:** Efficient insertion, deletion, and traversal of price levels.  
- **Orderbook Invariants:**  
  - No crossed orders (highest bid < lowest ask)  
  - Price-time priority enforced  
  - All order quantities non-negative and consistent  
- **Custom Arena Pools:** Optimized memory allocation for skiplist nodes and high-frequency operations.  

---

## Roadmap / Vision

- **Python Integration:** Connect orderbooks to Python for market simulation.  
- **Multivariate Hawkes Processes:** Model realistic correlated trading activity.  
- **UDP Multicast Feed:** Emit exchange-like events.  
- **TCP Connectivity:** Submit orders and query state in real-time.  
- **Analytics & Visualization:** Study orderbook dynamics and liquidity.  

---

## Example

```cpp
MatchingEngine engine{};

engine.SubmitOrder("AAPL", 105, 500, Side::Buy);
engine.DisplayBook("AAPL");

```
