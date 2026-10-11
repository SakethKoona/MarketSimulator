"""The researcher's side: the simulated world, its agents, and Scenarios.

A Scenario is a seeded, reproducible market: which symbols, which
population of agents, how the latent fundamental value moves, and what
shocks happen when. Run one as the market and let traders attach:

    python3 -m mktsim.market run hawkes --symbols AAPL,MSFT --seed 7
    python3 -m mktsim.market list

or from code: `SCENARIOS["hawkes"].run(workers=4)`.
"""
from __future__ import annotations

import multiprocessing as mp
import random
from dataclasses import dataclass, field
from typing import Callable, Optional

from .feed import Feed
from .trade import Market, Strategy


# ---------------------------------------------------------------- world

class Fundamental:
    """A latent value per symbol following a random walk, optionally mean
    reverting, with scripted shocks. Agents that 'know' it read it here."""

    def __init__(self, start: dict[str, int], vol: float = 1.0, revert: float = 0.0,
                 drift: float = 0.0, rng: random.Random | None = None):
        self.value = dict(start)
        self.anchor = dict(start)
        self.vol = vol          # std of the per-step move, in price units
        self.revert = revert    # fraction of the gap to the anchor closed per step
        self.drift = drift      # per-step drift in price units
        self.rng = rng or random.Random()

    def step(self) -> None:
        for s, v in self.value.items():
            move = self.rng.gauss(self.drift, self.vol)
            if self.revert:
                move += self.revert * (self.anchor[s] - v)
            self.value[s] = max(1, int(round(v + move)))

    def shock(self, sym: str, delta: int) -> None:
        self.value[sym] = max(1, self.value[sym] + delta)
        self.anchor[sym] = self.value[sym]

    def __getitem__(self, sym: str) -> int:
        return self.value[sym]


class World(Market):
    """The private simulation state: everything a Market is, plus the
    fundamental and scripted events. Only simulator agents get this."""

    def __init__(self, symbols: list[str], feed: Optional[Feed] = None,
                 fundamental: Optional[Fundamental] = None, seed: int = 1):
        super().__init__(symbols, feed)
        self.rng = random.Random(seed)
        self.fundamental = fundamental or Fundamental({s: 10000 for s in symbols}, rng=self.rng)
        self._events: list[tuple[float, Callable[["World"], None]]] = []

    def marks(self) -> dict[str, int]:
        """Mid from the feed, else the fundamental."""
        out = {}
        for s in self.symbols:
            m = self.mid(s)
            out[s] = m if m is not None else self.fundamental[s]
        return out

    def at(self, seconds: float, fn: Callable[["World"], None]) -> None:
        """Run fn(world) once the clock passes `seconds`."""
        self._events.append((seconds, fn))
        self._events.sort(key=lambda e: e[0])

    def shock(self, seconds: float, sym: str, delta: int) -> None:
        self.at(seconds, lambda w: w.fundamental.shock(sym, delta))

    def _tick(self) -> None:
        self.fundamental.step()
        now = self.now()
        while self._events and self._events[0][0] <= now:
            _, fn = self._events.pop(0)
            fn(self)


# ---------------------------------------------------------------- scenarios

@dataclass
class Scenario:
    """A reproducible market. `population(symbols, seed)` returns the agents;
    `fundamental` are Fundamental() kwargs; `shocks` are (seconds, sym, delta)."""

    name: str
    description: str
    population: Callable[[list[str], int], list[Strategy]]
    fundamental: dict = field(default_factory=dict)
    shocks: list[tuple[float, str, int]] = field(default_factory=list)
    symbols: list[str] = field(default_factory=lambda: ["AAPL", "MSFT", "GOOG"])
    seed: int = 1
    start: int = 10000

    def build(self, feed: Optional[Feed] = None, symbols=None, seed=None):
        symbols = list(symbols or self.symbols)
        seed = self.seed if seed is None else seed
        rng = random.Random(seed)
        fund = Fundamental({s: self.start for s in symbols}, rng=rng, **self.fundamental)
        world = World(symbols, feed=feed, fundamental=fund, seed=seed)
        for t, sym, delta in self.shocks:
            if sym in symbols:
                world.shock(t, sym, delta)
        agents = self.population(symbols, seed)
        return world, agents

    def run(self, host="127.0.0.1", port=30020, feed: Optional[Feed] | bool = True,
            symbols=None, seed=None, duration=None, workers: int = 1, report_every=10.0):
        """Runs the population as the market. workers>1 splits the agents
        over that many processes (each with its own feed subscription and
        its own copy of the world, same seed)."""
        from .runner import run_many  # late import: runner imports trade/sim
        symbols = list(symbols or self.symbols)
        seed = self.seed if seed is None else seed
        if workers <= 1:
            f = Feed() if feed is True else feed
            world, agents = self.build(f if f else None, symbols, seed)
            print(f"scenario {self.name}: {len(agents)} agents on {', '.join(symbols)}, seed {seed}")
            return run_many(agents, host=host, port=port, feed=f, world=world,
                            duration=duration, report_every=report_every)

        feed_kw = {} if feed is True else {"group": feed.group, "port": feed.port, "iface": feed.iface,
                                           "snapshot_host": feed.snapshot_host, "snapshot_port": feed.snapshot_port}
        ctx = mp.get_context("fork")
        procs = []
        for w in range(workers):
            p = ctx.Process(target=_worker, name=f"{self.name}-{w}",
                            args=(self, w, workers, host, port, feed_kw, symbols, seed, duration, report_every))
            p.start()
            procs.append(p)
        print(f"scenario {self.name}: {workers} workers on {', '.join(symbols)}, seed {seed}")
        try:
            for p in procs:
                p.join()
        except KeyboardInterrupt:
            for p in procs:
                p.terminate()
            for p in procs:
                p.join()
        return None


def _worker(scn: Scenario, w: int, n: int, host, port, feed_kw, symbols, seed, duration, report_every):
    from .runner import run_many
    f = Feed(**feed_kw)
    world, agents = scn.build(f, symbols, seed)
    mine = [a for i, a in enumerate(agents) if i % n == w]
    for a in mine:
        a.name = f"{a.name}@{w}"
    run_many(mine, host=host, port=port, feed=f, world=world, duration=duration,
             report_every=report_every if w == 0 else None)


# ---- the predefined flows ----

def _pop(noise, makers=1, momentum=0, informed=0, rate=1.0, cluster=0.5):
    def build(symbols, seed):
        from .agents import population
        return population(symbols, noise=noise, makers=makers, momentum=momentum,
                          informed=informed, seed=seed, noise_rate=rate, noise_cluster=cluster)
    return build


SCENARIOS: dict[str, Scenario] = {
    "random": Scenario(
        "random", "Poisson noise traders and a market maker per symbol; no clustering, no news",
        _pop(noise=8, makers=1, cluster=0.0), fundamental={"vol": 0.5}),
    "hawkes": Scenario(
        "hawkes", "self-exciting (Hawkes) noise flow: activity clusters and feeds on itself",
        _pop(noise=8, makers=1, cluster=0.6, rate=1.5), fundamental={"vol": 0.8}),
    "trending": Scenario(
        "trending", "a drifting fundamental with informed traders pushing price toward it",
        _pop(noise=6, makers=1, momentum=2, informed=2, cluster=0.4),
        fundamental={"vol": 1.0, "drift": 0.3}),
    "volatile": Scenario(
        "volatile", "high volatility fundamental with scheduled news shocks",
        _pop(noise=10, makers=2, momentum=2, informed=1, cluster=0.5, rate=2.0),
        fundamental={"vol": 3.0, "revert": 0.02},
        shocks=[(20.0, "AAPL", +80), (45.0, "MSFT", -60), (70.0, "AAPL", -50)]),
    "load": Scenario(
        "load", "many fast agents per symbol; use workers>1 for thousands of orders a second",
        _pop(noise=30, makers=2, momentum=2, informed=2, cluster=0.5, rate=8.0),
        fundamental={"vol": 1.0}),
}
