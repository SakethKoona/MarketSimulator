"""A small library of participants to build a market from, or to trade
against. Each is a Strategy; subclass to change behaviour.

    population(["AAPL"], noise=20, makers=2, momentum=3, informed=2, seed=7)
"""
from __future__ import annotations

import random

from .hawkes import Hawkes, Poisson
from .trade import Strategy


class NoiseTrader(Strategy):
    """Uninformed flow. Arrivals follow a Hawkes process so activity clusters;
    trades on the public feed excite it further when a feed is present.
    Orders are limits scattered around the mid, some aggressive."""

    def __init__(self, sym: str, rate: float = 1.0, cluster: float = 0.5, size=(1, 200),
                 spread_ticks: int = 15, aggressive: float = 0.2, seed: int = 0, name="",
                 max_live: int = 20):
        self.sym = sym
        self.rng = random.Random(seed)
        # cluster in [0,1): share of the rate that is self-excited
        beta = 2.0
        alpha = cluster * beta
        mu = rate * (1 - cluster)
        self.arrivals = Hawkes(mu, alpha, beta, self.rng) if cluster > 0 else Poisson(rate, self.rng)
        self.size = size
        self.spread_ticks = spread_ticks
        self.aggressive = aggressive
        self.name = name or f"noise{seed}"
        self.tick_ms = 20
        self.next_t = 0.0
        self.fallback_mid = 10000
        self.max_live = max_live

    def on_start(self):
        self.next_t = self.arrivals.next(self.world.now())

    def on_trade(self, trade):
        b = self.world.book(self.sym)
        if b and trade.sym == b.symbol_id:
            self.arrivals.excite(self.world.now())

    def on_tick(self):
        now = self.world.now()
        n = 0
        while self.next_t <= now and n < 50:   # catch up, but never flood
            self.act()
            self.next_t = self.arrivals.next(self.next_t)
            n += 1
        if self.next_t <= now:
            self.next_t = self.arrivals.next(now)
        # keep the book bounded: cancel the oldest resting orders beyond max_live
        live = self.ex.orders(self.sym)
        for o in live[: max(0, len(live) - self.max_live)]:
            self.ex.cancel(o.id)

    def act(self):
        mid = self.mid(self.sym) or self.world.fundamental[self.sym] or self.fallback_mid
        side = self.rng.choice("BS")
        qty = self.rng.randint(*self.size)
        if self.rng.random() < self.aggressive:
            # cross the spread: market-ish IOC
            px = mid + 20 if side == "B" else max(1, mid - 20)
            (self.ex.buy if side == "B" else self.ex.sell)(self.sym, qty, px, tif="IOC")
        else:
            off = self.rng.randint(1, self.spread_ticks)
            px = mid - off if side == "B" else mid + off
            (self.ex.buy if side == "B" else self.ex.sell)(self.sym, qty, max(1, px))


class MarketMaker(Strategy):
    """Quotes both sides around a fair value (public mid if available, else its
    own estimate), leans against inventory, requotes when the value moves."""

    def __init__(self, sym: str, size: int = 100, half_spread: int = 5, lean: float = 0.02,
                 max_pos: int = 2000, name=""):
        self.sym = sym
        self.size, self.half_spread, self.lean, self.max_pos = size, half_spread, lean, max_pos
        self.est = 10000
        self.name = name or f"mm_{sym}"
        self.tick_ms = 50

    def fair(self) -> int:
        m = self.mid(self.sym)
        if m is not None:
            self.est = m
        pos = self.ex.position.get(self.sym, 0)
        return int(self.est - self.lean * pos)   # long -> quote lower

    def on_start(self):
        self.quote()

    def on_report(self, r):
        if r.kind == "exec":
            self.est += -1 if r.side == "B" else 1  # adverse-selection nudge

    def on_tick(self):
        self.quote()

    def quote(self):
        f = self.fair()
        pos = self.ex.position.get(self.sym, 0)
        bid, ask = f - self.half_spread, f + self.half_spread
        want = {"B": bid if pos < self.max_pos else None, "S": ask if pos > -self.max_pos else None}
        for o in self.ex.orders(self.sym):
            w = want[o.side]
            if w is None:
                self.ex.cancel(o.id)
            elif o.px != w:
                self.ex.modify(o.id, o.qty, w)
            want[o.side] = None  # one order per side
        if want["B"] is not None:
            self.ex.buy(self.sym, self.size, want["B"])
        if want["S"] is not None:
            self.ex.sell(self.sym, self.size, want["S"])


class MomentumTaker(Strategy):
    """Buys after the mid rises, sells after it falls. Needs the feed."""

    def __init__(self, sym: str, window: int = 10, threshold: int = 3, size: int = 50, seed=0, name=""):
        self.sym, self.window, self.threshold, self.size = sym, window, threshold, size
        self.hist: list[int] = []
        self.rng = random.Random(seed)
        self.name = name or f"momo{seed}"
        self.tick_ms = 200

    def on_tick(self):
        m = self.mid(self.sym)
        if m is None:
            return
        self.hist.append(m)
        if len(self.hist) > self.window:
            self.hist.pop(0)
        if len(self.hist) < self.window:
            return
        move = self.hist[-1] - self.hist[0]
        if move > self.threshold:
            self.ex.buy(self.sym, self.size, m + 20, tif="IOC")
        elif move < -self.threshold:
            self.ex.sell(self.sym, self.size, max(1, m - 20), tif="IOC")


class InformedTrader(Strategy):
    """Knows the fundamental. Trades toward it when the market is mispriced,
    in small clips so as not to reveal everything at once."""

    def __init__(self, sym: str, edge: int = 4, clip: int = 50, max_pos: int = 3000, name=""):
        self.sym, self.edge, self.clip, self.max_pos = sym, edge, clip, max_pos
        self.name = name or f"informed_{sym}"
        self.tick_ms = 100

    def on_tick(self):
        m = self.mid(self.sym)
        if m is None:
            return
        v = self.world.fundamental[self.sym]
        pos = self.ex.position.get(self.sym, 0)
        if v - m > self.edge and pos < self.max_pos:
            self.ex.buy(self.sym, self.clip, m + self.edge, tif="IOC")
        elif m - v > self.edge and pos > -self.max_pos:
            self.ex.sell(self.sym, self.clip, max(1, m - self.edge), tif="IOC")


def population(symbols, noise=10, makers=1, momentum=0, informed=0, seed=1,
               noise_rate=1.0, noise_cluster=0.5):
    """A ready-made market: per symbol, `noise` Hawkes noise traders, `makers`
    market makers, `momentum` takers and `informed` traders."""
    rng = random.Random(seed)
    agents: list[Strategy] = []
    for sym in symbols:
        for i in range(noise):
            agents.append(NoiseTrader(sym, rate=noise_rate, cluster=noise_cluster,
                                      seed=rng.randrange(1 << 30), name=f"noise_{sym}_{i}"))
        for i in range(makers):
            agents.append(MarketMaker(sym, name=f"mm_{sym}_{i}"))
        for i in range(momentum):
            agents.append(MomentumTaker(sym, seed=rng.randrange(1 << 30), name=f"momo_{sym}_{i}"))
        for i in range(informed):
            agents.append(InformedTrader(sym, name=f"informed_{sym}_{i}"))
    return agents
