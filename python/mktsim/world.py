"""The shared world every strategy in a run can see: the clock, the public
books (from the feed, if subscribed), a latent fundamental value per symbol
that informed agents trade toward, and scripted events (shocks).
"""
from __future__ import annotations

import random
import time
from typing import Callable, Optional

from .feed import Book, Feed


class Fundamental:
    """A latent value per symbol following a random walk, optionally mean
    reverting, with scripted shocks. Agents that 'know' it read it here."""

    def __init__(self, start: dict[str, int], vol: float = 1.0, revert: float = 0.0,
                 rng: random.Random | None = None):
        self.value = dict(start)
        self.anchor = dict(start)
        self.vol = vol          # std of the per-step move, in price units
        self.revert = revert    # fraction of the gap to the anchor closed per step
        self.rng = rng or random.Random()

    def step(self) -> None:
        for s, v in self.value.items():
            move = self.rng.gauss(0, self.vol)
            if self.revert:
                move += self.revert * (self.anchor[s] - v)
            self.value[s] = max(1, int(round(v + move)))

    def shock(self, sym: str, delta: int) -> None:
        self.value[sym] = max(1, self.value[sym] + delta)
        self.anchor[sym] = self.value[sym]

    def __getitem__(self, sym: str) -> int:
        return self.value[sym]


class World:
    def __init__(self, symbols: list[str], feed: Optional[Feed] = None,
                 fundamental: Optional[Fundamental] = None, seed: int = 1):
        self.symbols = list(symbols)
        self.feed = feed
        self.rng = random.Random(seed)
        self.fundamental = fundamental or Fundamental({s: 10000 for s in symbols}, rng=self.rng)
        self.t0 = time.monotonic()
        self._events: list[tuple[float, Callable[["World"], None]]] = []

    # ---- time ----
    def now(self) -> float:
        """Seconds since the run started."""
        return time.monotonic() - self.t0

    # ---- market data ----
    def book(self, sym: str) -> Optional[Book]:
        return self.feed.book(sym) if self.feed else None

    def mid(self, sym: str) -> Optional[int]:
        b = self.book(sym)
        return b.mid() if b else None

    def marks(self) -> dict[str, int]:
        """Mark prices per symbol: mid from the feed, else the fundamental."""
        out = {}
        for s in self.symbols:
            m = self.mid(s)
            out[s] = m if m is not None else self.fundamental[s]
        return out

    # ---- scripted events ----
    def at(self, seconds: float, fn: Callable[["World"], None]) -> None:
        """Run fn(world) once the clock passes `seconds`."""
        self._events.append((seconds, fn))
        self._events.sort(key=lambda e: e[0])

    def shock(self, seconds: float, sym: str, delta: int) -> None:
        self.at(seconds, lambda w: w.fundamental.shock(sym, delta))

    def _tick(self) -> None:
        """Called by the runner every world tick."""
        self.fundamental.step()
        now = self.now()
        while self._events and self._events[0][0] <= now:
            _, fn = self._events.pop(0)
            fn(self)
