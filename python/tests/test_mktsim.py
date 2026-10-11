"""Offline tests for the Python client: feed decoding, Hawkes, report parsing.
Run: python3 -m unittest discover -s python/tests
"""
import json
import random
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mktsim.client import Exchange, Report  # noqa: E402
from mktsim.feed import MOLD_HDR, Books  # noqa: E402
from mktsim.hawkes import Hawkes, Poisson  # noqa: E402
from mktsim.sim import SCENARIOS, World  # noqa: E402
from mktsim.trade import Market, Strategy  # noqa: E402


def mold(seq, msgs, session=b"MKTSIM0001"):
    body = b"".join(struct.pack("<H", len(m)) + m for m in msgs)
    return MOLD_HDR.pack(session, seq, len(msgs)) + body


def add(sid, oid, side, px, qty, bseq, ts=0):
    return b"A" + struct.pack("<BIQQIQQ", ord(side), sid, oid, px, qty, bseq, ts)


def execd(sid, oid, side, px, eq, rem, match, bseq, ts=0):
    return b"E" + struct.pack("<BIQQIIQQQ", ord(side), sid, oid, px, eq, rem, match, bseq, ts)


def cancel(sid, oid, rem, bseq, ts=0):
    return b"X" + struct.pack("<IQIQQ", sid, oid, rem, bseq, ts)


def delete(sid, oid, bseq, ts=0):
    return b"D" + struct.pack("<IQQQ", sid, oid, bseq, ts)


def replace(sid, oid, side, px, qty, bseq, ts=0):
    return b"U" + struct.pack("<BIQQIQQ", ord(side), sid, oid, px, qty, bseq, ts)


def directory(sid, ticker, ts=0):
    return b"R" + struct.pack("<BHI", 0, 0, sid) + ticker.ljust(8).encode() + struct.pack("<Q", ts)


class FeedDecoding(unittest.TestCase):
    def test_sizes_match_spec(self):
        self.assertEqual(len(add(0, 1, "B", 1, 1, 1)), 42)
        self.assertEqual(len(execd(0, 1, "B", 1, 1, 0, 1, 1)), 54)
        self.assertEqual(len(cancel(0, 1, 1, 1)), 33)
        self.assertEqual(len(delete(0, 1, 1)), 29)
        self.assertEqual(len(replace(0, 1, "B", 1, 1, 1)), 42)
        self.assertEqual(len(directory(0, "AAPL")), 24)

    def test_book_lifecycle(self):
        b = Books()
        b.apply_packet(mold(1, [directory(0, "AAPL"), add(0, 1, "B", 100, 10, 1), add(0, 2, "S", 102, 5, 2),
                                add(0, 3, "S", 102, 7, 3)]))
        book = b.book("AAPL")
        self.assertEqual((book.best_bid(), book.best_ask()), (100, 102))
        self.assertEqual(book.depth("S"), [(102, 12, 2)])
        self.assertEqual(book.mid(), 101)
        # partial exec on order 2, aggressor buy
        b.apply_packet(mold(5, [execd(0, 2, "S", 102, 3, 2, 7, 4)]))
        self.assertEqual(book.depth("S"), [(102, 9, 2)])
        self.assertEqual(book.volume, 3)
        self.assertTrue(book.last.aggressor_buy)
        # full exec removes
        b.apply_packet(mold(6, [execd(0, 2, "S", 102, 2, 0, 8, 5)]))
        self.assertEqual(book.depth("S"), [(102, 7, 1)])
        # owner reduce, replace, delete
        b.apply_packet(mold(7, [cancel(0, 3, 4, 6), replace(0, 1, "B", 101, 20, 7)]))
        self.assertEqual(book.depth("S"), [(102, 4, 1)])
        self.assertEqual(book.depth("B"), [(101, 20, 1)])
        b.apply_packet(mold(9, [delete(0, 1, 8), delete(0, 3, 9)]))
        self.assertIsNone(book.best_bid())
        self.assertIsNone(book.best_ask())
        self.assertEqual(b.gaps, 0)

    def test_duplicate_and_gap(self):
        b = Books()
        p = mold(1, [add(0, 1, "B", 100, 10, 1)])
        b.apply_packet(p)
        b.apply_packet(p)  # macOS loopback double delivery
        self.assertEqual(b.dups, 1)
        self.assertEqual(b.book(0).depth("B"), [(100, 10, 1)])
        b.apply_packet(mold(10, [add(0, 2, "B", 99, 1, 2)]))
        self.assertEqual(b.gaps, 1)

    def test_snapshot_filters_older_live(self):
        b = Books()
        snap_start = b"Q" + struct.pack("<IQIQ", 0, 50, 1, 0)
        b.apply_packet(mold(0, [snap_start, add(0, 9, "S", 105, 3, 50)]), live=False)
        # a live message at or below the snapshot seq is ignored, above applies
        b.apply_packet(mold(100, [add(0, 10, "B", 90, 1, 40), add(0, 11, "B", 95, 2, 51)]))
        book = b.book(0)
        self.assertEqual(book.best_bid(), 95)
        self.assertEqual(book.best_ask(), 105)


class HawkesTests(unittest.TestCase):
    def test_rate_and_clustering(self):
        h = Hawkes(mu=1.0, alpha=0.5, beta=1.0, rng=random.Random(3))
        t, n, gaps = 0.0, 0, []
        while t < 2000:
            nt = h.next(t)
            gaps.append(nt - t)
            t = nt
            n += 1
        rate = n / t
        self.assertAlmostEqual(rate, h.mean_rate, delta=0.15 * h.mean_rate)
        # clustering: gap variance exceeds the Poisson value (mean^2)
        m = sum(gaps) / len(gaps)
        var = sum((g - m) ** 2 for g in gaps) / len(gaps)
        self.assertGreater(var, 1.3 * m * m)

    def test_poisson_rate(self):
        p = Poisson(5.0, random.Random(1))
        t, n = 0.0, 0
        while t < 1000:
            t = p.next(t)
            n += 1
        self.assertAlmostEqual(n / t, 5.0, delta=0.5)

    def test_stationarity_guard(self):
        with self.assertRaises(ValueError):
            Hawkes(1, 2, 1)


class ClientTracking(unittest.TestCase):
    def test_reports_update_orders_and_position(self):
        ex = Exchange(name="t")
        sent = []
        ex._send = lambda obj: sent.append(obj)
        oid = ex.buy("AAPL", 10, 100)
        self.assertEqual(sent[0]["new"]["px"], 100)
        ex.rx = b""
        feed = [{"ev": "accepted", "id": oid, "order": 7, "qty": 10, "px": 100, "leaves": 10},
                {"ev": "exec", "id": oid, "order": 7, "qty": 4, "px": 100, "leaves": 6, "match": 1},
                {"ev": "exec", "id": oid, "order": 7, "qty": 6, "px": 99, "leaves": 0, "match": 2}]

        class FakeSock:
            def __init__(self, lines): self.data = ("\n".join(json.dumps(l) for l in lines) + "\n").encode()
            def recv(self, n):
                d, self.data = self.data, b""
                return d
        ex.sock = FakeSock(feed)
        reps = list(ex.read())
        self.assertEqual([r.kind for r in reps], ["accepted", "exec", "exec"])
        self.assertEqual(reps[1].side, "B")
        self.assertEqual(reps[1].sym, "AAPL")
        self.assertEqual(ex.position["AAPL"], 10)
        self.assertEqual(ex.cash, -(4 * 100 + 6 * 99))
        self.assertEqual(ex.open, {})
        self.assertEqual(ex.pnl({"AAPL": 100}), 6)

    def test_market_order_defaults_to_ioc(self):
        ex = Exchange(name="t")
        sent = []
        ex._send = lambda obj: sent.append(obj)
        ex.sell("AAPL", 5)
        self.assertEqual(sent[0]["new"]["type"], "MARKET")
        self.assertEqual(sent[0]["new"]["tif"], "IOC")


class Scenarios(unittest.TestCase):
    def test_registry_builds_reproducibly(self):
        for name, scn in SCENARIOS.items():
            w1, a1 = scn.build(None, ["AAPL", "MSFT"], seed=3)
            w2, a2 = scn.build(None, ["AAPL", "MSFT"], seed=3)
            self.assertGreater(len(a1), 0, name)
            self.assertEqual([a.name for a in a1], [a.name for a in a2], name)
            self.assertEqual(w1.fundamental.value, w2.fundamental.value)
            self.assertTrue(all(isinstance(a, Strategy) for a in a1))

    def test_world_is_private_market_is_public(self):
        w = World(["AAPL"], seed=1)
        self.assertTrue(isinstance(w, Market))
        self.assertTrue(hasattr(w, "fundamental"))
        m = Market(["AAPL"])
        self.assertFalse(hasattr(m, "fundamental"))
        self.assertEqual(m.marks(), {"AAPL": 0})       # nothing public yet
        self.assertEqual(w.marks(), {"AAPL": 10000})   # falls back to the fundamental
        fired = []
        w.at(0.0, lambda ww: fired.append(ww.fundamental["AAPL"]))
        w._tick()
        self.assertEqual(len(fired), 1)


if __name__ == "__main__":
    unittest.main()
