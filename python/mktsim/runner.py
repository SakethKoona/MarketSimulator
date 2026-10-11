"""Drives one or many strategies in one process: one exchange session each,
one select loop, a shared World, an optional feed subscription."""
from __future__ import annotations

import select
import time
from typing import Callable, Iterable, Optional

from .client import Exchange
from .feed import Feed
from .strategy import Strategy
from .world import World


class _FnStrategy(Strategy):
    """Adapts three bare functions to the Strategy interface."""

    def __init__(self, on_start=None, on_report=None, on_tick=None, tick_ms=100):
        self._s, self._r, self._t = on_start, on_report, on_tick
        self.tick_ms = tick_ms
        self.name = "fn"

    def on_start(self):
        if self._s: self._s(self.ex)

    def on_report(self, r):
        if self._r: self._r(self.ex, r)

    def on_tick(self):
        if self._t: self._t(self.ex)


def run_many(strategies: Iterable[Strategy], host="127.0.0.1", port=30020,
             feed: Optional[Feed] | bool = True, world: Optional[World] = None,
             duration: Optional[float] = None, world_tick_ms: int = 100,
             report_every: Optional[float] = 10.0,
             on_summary: Optional[Callable[[list[Strategy], World], None]] = None) -> World:
    """Runs every strategy on its own exchange session until Ctrl-C or
    `duration` seconds. `feed=True` subscribes to the public feed with
    defaults; pass a Feed for other addresses; False for none."""
    strategies = list(strategies)
    if feed is True:
        feed = Feed()
    if feed:
        try:
            feed.open(snapshot=True)
        except OSError as e:
            print(f"feed: not available ({e}); running without market data")
            feed = None
    syms = sorted({getattr(s, "sym", None) for s in strategies} - {None})
    if world is None:
        world = World(syms or ["AAPL"], feed=feed)
    elif feed and world.feed is None:
        world.feed = feed

    # connect each strategy on its own session
    for i, s in enumerate(strategies):
        s.name = s.name or f"{type(s).__name__}{i}"
        s.ex = Exchange(host, port, s.name)
        s.world = world
        s.ex.connect()
    print(f"run: {len(strategies)} strateg{'y' if len(strategies) == 1 else 'ies'} on {host}:{port}"
          + (f", feed {feed.group}:{feed.port}" if feed else ", no feed"))

    if feed:
        def _on_trade(tr):
            for s in strategies:
                s.on_trade(tr)
        feed.books.on_trade = _on_trade

    for s in strategies:
        s.on_start()

    by_fd = {s.ex.fileno(): s for s in strategies}
    next_tick = {s: time.monotonic() + s.tick_ms / 1000 for s in strategies}
    next_world = time.monotonic() + world_tick_ms / 1000
    next_report = time.monotonic() + (report_every or 1e18)
    deadline = time.monotonic() + duration if duration else None
    fds = list(by_fd) + ([feed.fileno()] if feed else [])

    try:
        while True:
            now = time.monotonic()
            soonest = min([next_world] + list(next_tick.values()) + ([deadline] if deadline else []))
            r, _, _ = select.select(fds, [], [], max(0.0, soonest - now))
            for fd in r:
                if feed and fd == feed.fileno():
                    feed.read()
                else:
                    s = by_fd[fd]
                    for rep in s.ex.read():
                        s.on_report(rep)
            now = time.monotonic()
            if now >= next_world:
                next_world = now + world_tick_ms / 1000
                world._tick()
            for s in strategies:
                if now >= next_tick[s]:
                    next_tick[s] = now + s.tick_ms / 1000
                    s.on_tick()
            if report_every and now >= next_report:
                next_report = now + report_every
                _summary(strategies, world, feed)
            if deadline and now >= deadline:
                break
    except KeyboardInterrupt:
        pass
    finally:
        for s in strategies:
            try:
                s.on_stop()
            except Exception as e:  # noqa: BLE001
                print(f"{s.name}: on_stop raised {e}")
        _summary(strategies, world, feed)
        if on_summary:
            on_summary(strategies, world)
        for s in strategies:
            s.ex.close()
        if feed:
            feed.close()
    return world


def run(strategy=None, on_report=None, on_tick=None, tick_ms=100, host="127.0.0.1",
        port=30020, feed=True, duration=None, **kw):
    """One strategy. Accepts a Strategy instance or three bare functions."""
    if not isinstance(strategy, Strategy):
        strategy = _FnStrategy(strategy, on_report, on_tick, tick_ms)
    return run_many([strategy], host=host, port=port, feed=feed, duration=duration, **kw)


def _summary(strategies, world, feed):
    marks = world.marks()
    t = world.now()
    print(f"[{t:7.1f}s] ", end="")
    if feed:
        parts = []
        for sym in world.symbols[:4]:
            b = feed.book(sym)
            if b and b.mid() is not None:
                parts.append(f"{sym} {b.best_bid()}/{b.best_ask()} vol {b.volume}")
        print(" · ".join(parts) + (f" · gaps {feed.books.gaps}" if feed.books.gaps else ""), end="  |  ")
    total_pnl = sum(s.ex.pnl(marks) for s in strategies)
    live = sum(len(s.ex.open) for s in strategies)
    print(f"{len(strategies)} agents, {live} live orders, pnl {total_pnl:+d}")
