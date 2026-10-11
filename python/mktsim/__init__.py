"""mktsim: Python client for the MarketSimulator exchange.

Trade against the simulated market, or be the market: run one strategy or a
whole population of agents, each on its own exchange session, with the
public feed decoded into books they can read.

    from mktsim import Exchange, Strategy, run, run_many, agents, World, Feed
"""
from .client import Exchange, Order, Report
from .feed import Book, Books, Feed, Trade
from .hawkes import Hawkes, Poisson
from .runner import run, run_many
from .strategy import Strategy
from .world import Fundamental, World
from . import agents

__all__ = ["Exchange", "Order", "Report", "Book", "Books", "Feed", "Trade", "Hawkes", "Poisson",
           "run", "run_many", "Strategy", "Fundamental", "World", "agents"]
