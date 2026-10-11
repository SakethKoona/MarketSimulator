"""Market data: subscribe to the exchange's multicast feed, bootstrap from
the snapshot server, and keep an L3 book per symbol.

Contract: docs/protocol/feed-v1.md (MoldUDP64, little-endian).
"""
from __future__ import annotations

import socket
import struct
from dataclasses import dataclass, field
from typing import Optional

MOLD_HDR = struct.Struct("<10sQH")          # session, sequence, count


@dataclass
class Trade:
    sym: int
    px: int
    qty: int
    match: int
    aggressor_buy: bool
    ts_ns: int


@dataclass
class Level:
    qty: int = 0
    count: int = 0


@dataclass
class Book:
    """One symbol's L3 book rebuilt from the feed."""
    symbol_id: int
    ticker: str = ""
    orders: dict[int, tuple[str, int, int]] = field(default_factory=dict)  # id -> (side, px, qty)
    bids: dict[int, Level] = field(default_factory=dict)
    asks: dict[int, Level] = field(default_factory=dict)
    last: Optional[Trade] = None
    volume: int = 0
    trades: int = 0
    book_seq: int = 0
    snapshot_seq: int = 0   # ignore live messages at or below this after a snapshot

    def best_bid(self):
        return max(self.bids) if self.bids else None

    def best_ask(self):
        return min(self.asks) if self.asks else None

    def mid(self):
        b, a = self.best_bid(), self.best_ask()
        if b is None and a is None:
            return self.last.px if self.last else None
        if b is None:
            return a
        if a is None:
            return b
        return (a + b) // 2

    def spread(self):
        b, a = self.best_bid(), self.best_ask()
        return None if b is None or a is None else a - b

    def depth(self, side: str, levels: int = 5):
        """[(price, qty, count)] best first."""
        lv = self.bids if side == "B" else self.asks
        prices = sorted(lv, reverse=(side == "B"))[:levels]
        return [(p, lv[p].qty, lv[p].count) for p in prices]

    # ---- mutations ----
    def _add(self, oid, side, px, qty):
        self.orders[oid] = (side, px, qty)
        lv = (self.bids if side == "B" else self.asks).setdefault(px, Level())
        lv.qty += qty
        lv.count += 1

    def _remove(self, oid):
        o = self.orders.pop(oid, None)
        if o is None:
            return None
        side, px, qty = o
        levels = self.bids if side == "B" else self.asks
        lv = levels.get(px)
        if lv:
            lv.qty -= qty
            lv.count -= 1
            if lv.count <= 0:
                del levels[px]
        return o

    def _shrink(self, oid, remaining):
        o = self.orders.get(oid)
        if o is None:
            return False
        side, px, qty = o
        lv = (self.bids if side == "B" else self.asks).get(px)
        if lv:
            lv.qty -= qty - remaining
        self.orders[oid] = (side, px, remaining)
        return True

    def reset(self):
        self.orders.clear()
        self.bids.clear()
        self.asks.clear()


class Books:
    """All symbols. Decodes feed messages and applies them."""

    def __init__(self):
        self.by_id: dict[int, Book] = {}
        self.by_ticker: dict[str, Book] = {}
        self.session = b""
        self.next_seq = 0        # next expected Mold sequence, 0 = unknown
        self.gaps = 0
        self.dups = 0
        self.messages = 0
        self.on_trade = None     # callback(Trade) if set

    def book(self, sym) -> Optional[Book]:
        if isinstance(sym, str):
            return self.by_ticker.get(sym)
        return self.by_id.get(sym)

    def _get(self, sid: int) -> Book:
        b = self.by_id.get(sid)
        if b is None:
            b = self.by_id[sid] = Book(sid)
        return b

    # ---- packets ----
    def apply_packet(self, data: bytes, live: bool = True) -> None:
        if len(data) < MOLD_HDR.size:
            return
        session, seq, count = MOLD_HDR.unpack_from(data, 0)
        if live:
            if count == 0xFFFF:
                return  # end of session
            if self.next_seq and seq < self.next_seq:
                # Duplicate delivery (macOS loopback) or overlap: skip what we have
                skip = self.next_seq - seq
                if skip >= count:
                    self.dups += 1
                    return
            elif self.next_seq and seq > self.next_seq:
                self.gaps += 1
                for b in self.by_id.values():
                    b.reset()   # unrecoverable without a snapshot; caller may resnapshot
            self.session = session
        off = MOLD_HDR.size
        n = 0
        while n < count and off + 2 <= len(data):
            (ln,) = struct.unpack_from("<H", data, off)
            off += 2
            msg = data[off:off + ln]
            off += ln
            if live and self.next_seq and seq + n < self.next_seq:
                n += 1
                continue  # already applied
            self.apply_message(msg, live)
            n += 1
        if live:
            self.next_seq = seq + count if count != 0xFFFF else self.next_seq

    # ---- messages ----
    def apply_message(self, m: bytes, live: bool = True) -> None:
        if not m:
            return
        t = m[0:1]
        self.messages += 1
        if t == b"A":
            side, sid, oid, px, qty, bseq = struct.unpack_from("<BIQQIQ", m, 1)
            b = self._get(sid)
            if live and bseq <= b.snapshot_seq:
                return
            b.book_seq = bseq
            b._add(oid, "B" if side == ord("B") else "S", px, qty)
        elif t == b"E":
            side, sid, oid, px, eq, rem, match, bseq, ts = struct.unpack_from("<BIQQIIQQQ", m, 1)
            b = self._get(sid)
            if live and bseq <= b.snapshot_seq:
                return
            b.book_seq = bseq
            if rem == 0:
                b._remove(oid)
            else:
                b._shrink(oid, rem)
            tr = Trade(sid, px, eq, match, aggressor_buy=(side == ord("S")), ts_ns=ts)
            b.last = tr
            b.volume += eq
            b.trades += 1
            if self.on_trade:
                self.on_trade(tr)
        elif t == b"X":
            sid, oid, rem, bseq = struct.unpack_from("<IQIQ", m, 1)
            b = self._get(sid)
            if live and bseq <= b.snapshot_seq:
                return
            b.book_seq = bseq
            b._shrink(oid, rem)
        elif t == b"D":
            sid, oid, bseq = struct.unpack_from("<IQQ", m, 1)
            b = self._get(sid)
            if live and bseq <= b.snapshot_seq:
                return
            b.book_seq = bseq
            b._remove(oid)
        elif t == b"U":
            side, sid, oid, px, qty, bseq = struct.unpack_from("<BIQQIQ", m, 1)
            b = self._get(sid)
            if live and bseq <= b.snapshot_seq:
                return
            b.book_seq = bseq
            b._remove(oid)
            b._add(oid, "B" if side == ord("B") else "S", px, qty)
        elif t == b"R":
            _scale, _res, sid = struct.unpack_from("<BHI", m, 1)
            ticker = m[8:16].decode("ascii", "replace").strip()
            b = self._get(sid)
            if b.ticker != ticker:
                b.ticker = ticker
                self.by_ticker[ticker] = b
        elif t == b"Q":   # snapshot start: replace the book
            sid, bseq, _count = struct.unpack_from("<IQI", m, 1)
            b = self._get(sid)
            b.reset()
            b.snapshot_seq = bseq
            b.book_seq = bseq
        # 'S' system event, 'Z' snapshot end: nothing to do


class Feed:
    """Multicast subscriber + snapshot bootstrap. Call fileno()/read() from
    a select loop, or poll()."""

    def __init__(self, group="239.1.1.1", port=30001, iface="127.0.0.1",
                 snapshot_host="127.0.0.1", snapshot_port=30003, session="MKTSIM0001"):
        self.group, self.port, self.iface = group, port, iface
        self.snapshot_host, self.snapshot_port = snapshot_host, snapshot_port
        self.session = session.ljust(10)[:10].encode()
        self.books = Books()
        self.sock = None
        self._pending: list[bytes] = []

    def open(self, snapshot=True) -> "Feed":
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        if hasattr(socket, "SO_REUSEPORT"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
        s.bind(("", self.port))
        mreq = socket.inet_aton(self.group) + socket.inet_aton(self.iface or "0.0.0.0")
        s.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
        s.setblocking(False)
        self.sock = s
        if snapshot:
            self.snapshot()
        return self

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def fileno(self):
        return self.sock.fileno()

    def read(self) -> int:
        """Drains the socket; returns packets applied."""
        n = 0
        while True:
            try:
                data = self.sock.recv(65536)
            except BlockingIOError:
                return n
            self.books.apply_packet(data, live=True)
            n += 1

    def snapshot(self, symbol_id=0xFFFFFFFF) -> bool:
        """Loads books from the snapshot server. Live packets that arrive
        meanwhile are applied afterwards and filtered by book_seq."""
        try:
            c = socket.create_connection((self.snapshot_host, self.snapshot_port), timeout=5)
        except OSError:
            return False
        with c:
            c.sendall(struct.pack("<10sI", self.session, symbol_id))
            buf = b""
            while True:
                try:
                    chunk = c.recv(65536)
                except socket.timeout:
                    break
                if not chunk:
                    break
                buf += chunk
        # Snapshot replies are Mold packets back to back, seq 0
        off = 0
        while off + MOLD_HDR.size <= len(buf):
            _s, _seq, count = MOLD_HDR.unpack_from(buf, off)
            end = off + MOLD_HDR.size
            for _ in range(count):
                if end + 2 > len(buf):
                    break
                (ln,) = struct.unpack_from("<H", buf, end)
                end += 2 + ln
            self.books.apply_packet(buf[off:end], live=False)
            off = end
        self.books.gaps = 0
        return True

    # convenience
    def book(self, sym) -> Optional[Book]:
        return self.books.book(sym)
