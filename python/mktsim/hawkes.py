"""Univariate Hawkes process with an exponential kernel, for self-exciting
order arrivals. Intensity lambda(t) = mu + sum_i alpha * exp(-beta (t - t_i)).

    h = Hawkes(mu=2.0, alpha=0.8, beta=1.5, rng=random.Random(1))
    t = h.next(t)          # next arrival time after t (Ogata thinning)
    h.excite(t_ext)        # an external event (e.g. a trade on the feed) also excites

Stationary when alpha < beta. Mean rate = mu / (1 - alpha / beta).
"""
from __future__ import annotations

import math
import random


class Hawkes:
    def __init__(self, mu: float, alpha: float, beta: float, rng: random.Random | None = None,
                 max_intensity: float | None = None, external_alpha: float | None = None):
        if alpha >= beta:
            raise ValueError("Hawkes needs alpha < beta to be stationary")
        self.mu, self.alpha, self.beta = mu, alpha, beta
        self.rng = rng or random.Random()
        # Externally driven excitation (excite()) is not bounded by the
        # stationarity condition, so cap the intensity and let external
        # events count for less than own arrivals.
        self.max_intensity = max_intensity if max_intensity is not None else 20 * max(mu, 1e-9)
        self.external_alpha = external_alpha if external_alpha is not None else alpha * 0.1
        self._excess = 0.0   # sum of alpha * exp(-beta (t - t_i)) at self._t
        self._t = 0.0

    def intensity(self, t: float) -> float:
        return self.mu + self._excess * math.exp(-self.beta * (t - self._t))

    def _advance(self, t: float) -> None:
        self._excess *= math.exp(-self.beta * (t - self._t))
        self._t = t

    def excite(self, t: float) -> None:
        """Register an external event at time t (e.g. a trade on the feed)."""
        self._advance(t)
        self._excess = min(self._excess + self.external_alpha, self.max_intensity - self.mu)

    def next(self, t: float) -> float:
        """Next arrival strictly after t; registers it as an own event."""
        self._advance(t)
        while True:
            lam_bar = min(self.mu + self._excess, self.max_intensity)  # can only decay
            t += self.rng.expovariate(lam_bar)
            self._advance(t)
            if self.rng.random() * lam_bar <= min(self.mu + self._excess, self.max_intensity):
                self._excess = min(self._excess + self.alpha, self.max_intensity - self.mu)
                return t

    @property
    def mean_rate(self) -> float:
        return self.mu / (1 - self.alpha / self.beta)


class Poisson:
    """Same interface, no self-excitation."""

    def __init__(self, rate: float, rng: random.Random | None = None):
        self.rate = rate
        self.rng = rng or random.Random()

    def excite(self, t: float) -> None:
        pass

    def next(self, t: float) -> float:
        return t + self.rng.expovariate(self.rate)

    @property
    def mean_rate(self) -> float:
        return self.rate
