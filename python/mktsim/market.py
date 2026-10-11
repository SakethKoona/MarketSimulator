"""Run a predefined market scenario as the order flow.

    python3 -m mktsim.market list
    python3 -m mktsim.market run hawkes [--symbols AAPL,MSFT] [--seed 7] [--workers 4]
                                        [--port 30020] [--feed 239.1.1.1:30001] [--snapshot-port 30003]
                                        [--duration S] [--iface 127.0.0.1]
"""
from __future__ import annotations

import argparse
import sys

from .feed import Feed
from .sim import SCENARIOS


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python3 -m mktsim.market")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="list predefined scenarios")
    r = sub.add_parser("run", help="run a scenario as the market")
    r.add_argument("name", choices=sorted(SCENARIOS))
    r.add_argument("--symbols", default=None, help="comma separated; default: the scenario's")
    r.add_argument("--seed", type=int, default=None)
    r.add_argument("--workers", type=int, default=1, help="processes to split the agents over")
    r.add_argument("--host", default="127.0.0.1")
    r.add_argument("--port", type=int, default=30020, help="the exchange's JSON-lines order entry port")
    r.add_argument("--feed", default="239.1.1.1:30001", help="multicast group:port")
    r.add_argument("--iface", default="127.0.0.1")
    r.add_argument("--snapshot-port", type=int, default=30003)
    r.add_argument("--duration", type=float, default=None, help="seconds; default: until Ctrl-C")
    r.add_argument("--report-every", type=float, default=10.0)
    a = ap.parse_args(argv)

    if a.cmd == "list":
        w = max(len(n) for n in SCENARIOS)
        for n, s in SCENARIOS.items():
            print(f"  {n:<{w}}  {s.description}")
        return 0

    group, port = a.feed.split(":")
    feed = Feed(group=group, port=int(port), iface=a.iface, snapshot_port=a.snapshot_port)
    symbols = [s.strip().upper() for s in a.symbols.split(",")] if a.symbols else None
    SCENARIOS[a.name].run(host=a.host, port=a.port, feed=feed, symbols=symbols, seed=a.seed,
                          duration=a.duration, workers=a.workers, report_every=a.report_every)
    return 0


if __name__ == "__main__":
    sys.exit(main())
