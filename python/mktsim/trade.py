"""The trader's side: a Strategy and the Market it is allowed to see.

A Market is the public view: the clock and the books rebuilt from the
exchange's feed. Nothing private to the simulation (the fundamental value,
other agents' intentions) is reachable from here, so a strategy tested
against a scenario sees exactly what it would see on a real venue.
"""
from __future__ import annotations

import time
from typing import Optional

from .client import Exchange, Report
from .feed import Book, Feed, Trade


class Market:
    """Public market view: clock + books from the feed."""

    def __init__(self, symbols: list[str], feed: Optional[Feed] = None):
        self.symbols = list(symbols)
        self.feed = feed
        self.t0 = time.monotonic()

    def now(self) -> float:
        """Seconds since the run started."""
        return time.monotonic() - self.t0

    def book(self, sym: str) -> Optional[Book]:
        return self.feed.book(sym) if self.feed else None

    def mid(self, sym: str) -> Optional[int]:
        b = self.book(sym)
        return b.mid() if b else None

    def spread(self, sym: str) -> Optional[int]:
        b = self.book(sym)
        return b.spread() if b else None

    def depth(self, sym: str, side: str, levels: int = 5):
        b = self.book(sym)
        return b.depth(side, levels) if b else []

    def last(self, sym: str) -> Optional[Trade]:
        b = self.book(sym)
        return b.last if b else None

    def marks(self) -> dict[str, int]:
        """Mark prices per symbol from the public mid (0 if unknown)."""
        return {s: (self.mid(s) or 0) for s in self.symbols}

    def _tick(self) -> None:
        """Called by the runner every world tick. Nothing to do publicly."""


class Strategy:
    """One participant. Subclass it, keep state on self, override the hooks.

    The runner sets self.ex (your exchange session) and self.market (the
    public view) before on_start. Simulator agents additionally get
    self.world (the private simulation state).

    tick_ms controls how often on_tick is called for this strategy.
    """

    tick_ms: int = 100
    name: str = ""

    ex: Exchange
    market: Market

    def on_start(self) -> None:
        """Connected. Place opening orders."""

    def on_report(self, r: Report) -> None:
        """An ack, fill, cancel, modify or reject on one of your orders."""

    def on_tick(self) -> None:
        """Every tick_ms. Requote, cancel stale orders, manage risk."""

    def on_trade(self, trade: Trade) -> None:
        """A trade on the public feed (any participant), if the run has a feed."""

    def on_stop(self) -> None:
        """The run is ending. Cancel what you must; the exchange keeps the rest."""

    # ---- conveniences ----
    def mid(self, sym: str) -> Optional[int]:
        m = getattr(self, "market", None)
        return m.mid(sym) if m else None

    def book(self, sym: str) -> Optional[Book]:
        m = getattr(self, "market", None)
        return m.book(sym) if m else None

    def pnl(self) -> int:
        return self.ex.pnl(self.market.marks())

    def __repr__(self) -> str:
        return f"{type(self).__name__}({self.name})"
