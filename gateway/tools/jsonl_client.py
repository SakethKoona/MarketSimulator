#!/usr/bin/env python3
"""Tiny client for the jsonl ingress adapter (plugins/jsonl_ingress.cpp).

  jsonl_client.py demo                       rest a bid and an ask, cross them, modify, cancel, reject
  jsonl_client.py new B AAPL 100 10000 [GTC|IOC|FOK]
Environment: JSONL_HOST, JSONL_PORT (default 127.0.0.1:30020).
"""
import json, os, socket, sys, time

class Client:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port))
        self.buf = b""

    def send(self, obj):
        line = json.dumps(obj)
        print("->", line)
        self.s.sendall(line.encode() + b"\n")

    def recv(self, timeout=0.5):
        """Print every complete line until `timeout` passes with nothing new."""
        self.s.settimeout(timeout)
        while True:
            while b"\n" in self.buf:
                line, _, self.buf = self.buf.partition(b"\n")
                print("<-", line.decode())
            try:
                chunk = self.s.recv(65536)
            except (socket.timeout, TimeoutError):
                return
            if not chunk:
                return
            self.buf += chunk

def main():
    host = os.environ.get("JSONL_HOST", "127.0.0.1")
    port = int(os.environ.get("JSONL_PORT", "30020"))
    args = sys.argv[1:] or ["demo"]
    c = Client(host, port)
    if args[0] == "demo":
        c.send({"ping": 1}); c.recv()
        c.send({"new": {"id": "bid1", "sym": "AAPL", "side": "B", "qty": 100, "px": 9990}})
        c.send({"new": {"id": "ask1", "sym": "AAPL", "side": "S", "qty": 50, "px": 10010}})
        c.recv()
        c.send({"new": {"id": "ioc1", "sym": "AAPL", "side": "B", "qty": 80, "px": 10020, "tif": "IOC"}})
        c.recv()
        c.send({"modify": {"id": "bid1", "qty": 60}}); c.recv()
        c.send({"cancel": {"id": "bid1"}}); c.recv()
        c.send({"new": {"id": "bad", "sym": "ZZZZ", "side": "B", "qty": 1, "px": 1}}); c.recv()
    elif args[0] == "new":
        side, sym, qty, px = args[1], args[2], int(args[3]), int(args[4])
        tif = args[5] if len(args) > 5 else "GTC"
        c.send({"new": {"id": f"o{int(time.time())%100000}", "sym": sym, "side": side, "qty": qty, "px": px, "tif": tif}})
        c.recv(1.0)
    else:
        print(__doc__); sys.exit(2)

if __name__ == "__main__":
    main()
