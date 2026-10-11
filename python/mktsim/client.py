"""Order entry: one TCP connection to the exchange's JSON-lines adapter = one
exchange session. A Strategy talks to the exchange through an Exchange.

Wire format (one JSON object per line):
  -> {"new":{"id":"c1","sym":"AAPL","side":"B","qty":100,"px":10000,"tif":"GTC"}}
  -> {"cancel":{"id":"c1"}}   {"modify":{"id":"c1","qty":60,"px":10010}}
  <- {"ev":"accepted"|"rejected"|"exec"|"cancelled"|"modified", "id":..., ...}
"""
from __future__ import annotations

import json
import socket
from dataclasses import dataclass, field
from typing import Iterator, Optional


@dataclass
class Report:
    kind: str            # accepted | rejected | exec | cancelled | modified
    id: str              # the id you gave the order
    side: str = ""       # "B" or "S", from your order
    sym: str = ""        # symbol, from your order
    order: int = 0       # the exchange's order id (also on the public feed)
    qty: int = 0         # accepted/modified: order qty. exec: fill qty
    px: int = 0          # accepted/modified: order px. exec: fill px
    leaves: int = 0      # qty still resting after this event (exec: 0 = done)
    match: int = 0       # exec: match id
    reason: str = ""     # rejected
    raw: dict = field(default_factory=dict)


@dataclass
class Order:
    id: str
    sym: str
    side: str            # "B" or "S"
    qty: int
    px: Optional[int]    # None for a market order
    tif: str
    order: int = 0       # exchange id once accepted
    leaves: int = 0
    filled: int = 0


class Exchange:
    """One session on the exchange. Place orders, track what is working."""

    def __init__(self, host: str = "127.0.0.1", port: int = 30020, name: str = "py"):
        self.host, self.port, self.name = host, port, name
        self.sock: Optional[socket.socket] = None
        self.rx = b""
        self.open: dict[str, Order] = {}   # your live orders by id
        self.position: dict[str, int] = {} # signed shares per symbol, from fills
        self.cash = 0                      # from fills, before marking positions
        self._seq = 0

    # ---- orders ----
    def buy(self, sym, qty, price=None, tif="GTC", id=None) -> str:
        return self._new(sym, "B", qty, price, tif, id)

    def sell(self, sym, qty, price=None, tif="GTC", id=None) -> str:
        return self._new(sym, "S", qty, price, tif, id)

    def cancel(self, id: str) -> None:
        self._send({"cancel": {"id": id}})

    def modify(self, id: str, qty: int, price=None) -> None:
        m = {"id": id, "qty": int(qty)}
        if price is not None:
            m["px"] = int(price)
        self._send({"modify": m})

    def cancel_all(self, sym=None) -> None:
        for o in self.orders(sym):
            self.cancel(o.id)

    # ---- what's working ----
    def orders(self, sym=None, side=None) -> list[Order]:
        return [o for o in self.open.values()
                if (sym is None or o.sym == sym) and (side is None or o.side == side)]

    def has_side(self, side, sym=None) -> bool:
        return any(self.orders(sym, side))

    def _new(self, sym, side, qty, price, tif, id) -> str:
        self._seq += 1
        id = id or f"{self.name}-{self._seq}"
        o = {"id": id, "sym": sym, "side": side, "qty": int(qty), "tif": tif}
        if price is None:
            o["type"] = "MARKET"
            if tif == "GTC":
                o["tif"] = "IOC"
        else:
            o["px"] = int(price)
        self.open[id] = Order(id, sym, side, int(qty), price, o["tif"])
        self._send({"new": o})
        return id

    # ---- plumbing ----
    def connect(self) -> None:
        self.sock = socket.create_connection((self.host, self.port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.setblocking(False)

    def close(self) -> None:
        if self.sock:
            self.sock.close()
            self.sock = None

    def fileno(self) -> int:
        return self.sock.fileno()

    def _send(self, obj) -> None:
        self.sock.sendall((json.dumps(obj, separators=(",", ":")) + "\n").encode())

    def read(self) -> Iterator[Report]:
        """Reads what is available on the socket and yields Reports.
        Call when select() says the socket is readable."""
        data = self.sock.recv(65536)
        if not data:
            raise ConnectionError(f"{self.name}: exchange closed the connection")
        self.rx += data
        while b"\n" in self.rx:
            line, self.rx = self.rx.split(b"\n", 1)
            if not line.strip():
                continue
            ev = json.loads(line)
            kind = ev.get("ev", "")
            if kind in ("pong", "error"):
                if kind == "error":
                    print(f"{self.name}: exchange error: {ev.get('msg')}")
                continue
            rep = Report(kind=kind, id=ev.get("id", ""), order=ev.get("order", 0),
                         qty=ev.get("qty", 0), px=ev.get("px", 0), leaves=ev.get("leaves", 0),
                         match=ev.get("match", 0), reason=ev.get("reason", ""), raw=ev)
            self._track(rep)
            yield rep

    def _track(self, r: Report) -> None:
        o = self.open.get(r.id)
        if o is None:
            return
        r.side, r.sym = o.side, o.sym
        if r.kind == "accepted":
            o.order, o.leaves = r.order, r.leaves
        elif r.kind == "exec":
            o.filled += r.qty
            o.leaves = r.leaves
            signed = r.qty if o.side == "B" else -r.qty
            self.position[o.sym] = self.position.get(o.sym, 0) + signed
            self.cash -= signed * r.px
            if r.leaves == 0:
                self.open.pop(r.id, None)
        elif r.kind == "modified":
            o.qty, o.leaves = r.qty, r.leaves
            if r.px:
                o.px = r.px
        elif r.kind in ("cancelled", "rejected"):
            self.open.pop(r.id, None)

    def pnl(self, marks: dict[str, int]) -> int:
        """Cash plus positions marked at the given prices."""
        return self.cash + sum(q * marks.get(s, 0) for s, q in self.position.items())
