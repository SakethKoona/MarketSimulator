"""Strategy: subclass it, keep state on self, override the hooks."""
from __future__ import annotations

from typing import Optional

from .client import Exchange, Report
from .world import World


class Strategy:
    """One participant. The runner gives it an Exchange session (self.ex) and
    the shared World (self.world) before calling on_start.

    tick_ms controls how often on_tick is called for this strategy.
    """

    tick_ms: int = 100
    name: str = ""

    ex: Exchange
    world: World

    def on_start(self) -> None:
        """Connected. Place opening orders."""

    def on_report(self, r: Report) -> None:
        """An ack, fill, cancel, modify or reject on one of your orders."""

    def on_tick(self) -> None:
        """Every tick_ms. Requote, cancel stale orders, manage risk."""

    def on_trade(self, trade) -> None:
        """A trade on the public feed (any participant), if the run has a feed."""

    def on_stop(self) -> None:
        """The run is ending. Cancel what you must; the exchange keeps the rest."""

    # ---- conveniences ----
    def mid(self, sym: str) -> Optional[int]:
        """Best available mid: public book if there is a feed, else None."""
        return self.world.mid(sym) if getattr(self, "world", None) else None

    def pnl(self) -> int:
        return self.ex.pnl(self.world.marks())

    def __repr__(self) -> str:
        return f"{type(self).__name__}({self.name})"
