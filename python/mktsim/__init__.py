"""mktsim: Python client for the MarketSimulator exchange.

Two sides of one library:

  trade  - a Strategy and the public Market it sees. Trade against whatever
           market is running (the built-in demo flow or a researcher's
           scenario). `from mktsim import Strategy, run`.
  sim    - the simulation: World (private state: Fundamental, shocks),
           agents, and seeded Scenarios that run as the market.
           `from mktsim.sim import SCENARIOS, Scenario, World`;
           `python3 -m mktsim.market run hawkes`.

Both use the same Exchange session and the same Feed decoder.
"""
from .client import Exchange, Order, Report
from .feed import Book, Books, Feed, Trade
from .hawkes import Hawkes, Poisson
from .runner import run, run_many
from .sim import SCENARIOS, Fundamental, Scenario, World
from .trade import Market, Strategy
from . import agents, sim, trade

__all__ = ["Exchange", "Order", "Report", "Book", "Books", "Feed", "Trade", "Hawkes", "Poisson",
           "run", "run_many", "Strategy", "Market", "Fundamental", "World", "Scenario", "SCENARIOS",
           "agents", "sim", "trade"]
