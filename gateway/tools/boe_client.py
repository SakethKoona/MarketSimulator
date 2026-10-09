#!/usr/bin/env python3
"""Minimal BOE v1 client, a second independent implementation of
docs/protocol/boe-v1.md. Logs in, sends orders, prints every report.

  boe_client.py [--host H] [--port P] demo
  boe_client.py order BUY AAPL 100 105 [GTC|IOC|FOK]
"""
import argparse, socket, struct, sys, time

SOM = 0xBABA
HDR = struct.Struct('<HHBBI')          # start, length, type, unit, seq
LOGIN_REQ = struct.Struct('<4s4s10s')
LOGIN_RSP = struct.Struct('<BI60s')
LOGOUT = struct.Struct('<B60s')
NEW = struct.Struct('<20sBIQ8sBB')
CANCEL = struct.Struct('<20s')
MODIFY = struct.Struct('<20s20sIQ')
ACK = struct.Struct('<Q20sQ8sBIQI')
REJ = struct.Struct('<Q20sB60s')
MOD = struct.Struct('<Q20sQIQI')
CXL = struct.Struct('<Q20sQB')
EXE = struct.Struct('<Q20sQQIQIB')
assert (NEW.size, CANCEL.size, MODIFY.size, ACK.size, REJ.size, MOD.size, CXL.size, EXE.size) == (43, 20, 52, 61, 89, 52, 37, 61)

T = dict(LOGIN=0x01, LOGOUT=0x02, HB=0x03, NEW=0x04, CANCEL=0x05, MODIFY=0x06,
         LOGIN_RSP=0x07, LOGOUT_RSP=0x08, SHB=0x09, ACK=0x25, REJ=0x26, MOD=0x27, CXL=0x28, EXE=0x2C)
NAMES = {v: k for k, v in T.items()}

def pad(s, n): return s.encode()[:n].ljust(n, b' ')

class Client:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.out_seq = 1
        self.buf = b''

    def send(self, mtype, body=b'', app=True):
        seq = self.out_seq if app else 0
        if app: self.out_seq += 1
        self.s.sendall(HDR.pack(SOM, 6 + len(body), mtype, 0, seq) + body)

    def login(self, sub='0001', user='TEST', pw='pw'):
        self.send(T['LOGIN'], LOGIN_REQ.pack(pad(sub, 4), pad(user, 4), pad(pw, 10)), app=False)

    def new(self, cl, side, sym, qty, px, tif='GTC', typ='LIMIT'):
        self.send(T['NEW'], NEW.pack(pad(cl, 20), ord(side[0]), qty, px, pad(sym, 8),
                                     ord('1') if typ == 'MARKET' else ord('2'),
                                     {'GTC': ord('0'), 'IOC': ord('3'), 'FOK': ord('4')}[tif]))

    def cancel(self, cl): self.send(T['CANCEL'], CANCEL.pack(pad(cl, 20)))
    def modify(self, new_cl, orig_cl, qty, px=0): self.send(T['MODIFY'], MODIFY.pack(pad(new_cl, 20), pad(orig_cl, 20), qty, px))

    def recv(self, timeout=1.0):
        """Yield (type_name, dict) until timeout with no data."""
        self.s.settimeout(timeout)
        while True:
            while len(self.buf) >= HDR.size:
                som, length, mtype, _, seq = HDR.unpack_from(self.buf)
                assert som == SOM, 'bad start of message'
                if len(self.buf) < 4 + length: break
                body = self.buf[HDR.size:4 + length]
                self.buf = self.buf[4 + length:]
                yield NAMES.get(mtype, hex(mtype)), seq, self.decode(mtype, body)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return
            if not d: return
            self.buf += d

    @staticmethod
    def decode(mtype, b):
        f = lambda x: x.decode().strip()
        if mtype == T['LOGIN_RSP']:
            st, last, text = LOGIN_RSP.unpack(b); return dict(status=chr(st), last_seq=last, text=f(text))
        if mtype == T['LOGOUT_RSP']:
            r, text = LOGOUT.unpack(b); return dict(reason=chr(r), text=f(text))
        if mtype == T['ACK']:
            ts, cl, oid, sym, side, qty, px, leaves = ACK.unpack(b)
            return dict(cl=f(cl), order_id=oid, sym=f(sym), side=chr(side), qty=qty, px=px, leaves=leaves)
        if mtype == T['REJ']:
            ts, cl, reason, text = REJ.unpack(b); return dict(cl=f(cl), reason=chr(reason), text=f(text))
        if mtype == T['MOD']:
            ts, cl, oid, qty, px, leaves = MOD.unpack(b); return dict(cl=f(cl), order_id=oid, qty=qty, px=px, leaves=leaves)
        if mtype == T['CXL']:
            ts, cl, oid, reason = CXL.unpack(b); return dict(cl=f(cl), order_id=oid, reason=chr(reason))
        if mtype == T['EXE']:
            ts, cl, oid, mid, lq, lpx, leaves, side = EXE.unpack(b)
            return dict(cl=f(cl), order_id=oid, match=mid, last_qty=lq, last_px=lpx, leaves=leaves, side=chr(side))
        return dict(raw=b.hex())

def show(c, timeout=0.5):
    n = 0
    for name, seq, d in c.recv(timeout):
        print(f'  <- {name:9} seq={seq:<4} {d}'); n += 1
    return n

def demo(c):
    c.login(); show(c)
    print('-> resting bid, resting ask, then a crossing IOC buy')
    c.new('bid1', 'B', 'AAPL', 100, 9990)
    c.new('ask1', 'S', 'AAPL', 50, 10010)
    show(c)
    c.new('ioc1', 'B', 'AAPL', 80, 10020, 'IOC')      # fills 50 against ask1, 30 cancelled
    show(c)
    print('-> shrink bid1 to 60, then move it up (replace), then cancel')
    c.modify('bid1', 'bid1', 60)
    show(c)
    c.modify('bid2', 'bid1', 60, 9995)
    show(c)
    c.cancel('bid2')
    show(c)
    print('-> rejects: unknown symbol, zero qty, unknown order, duplicate cl_ord_id, FOK unfillable')
    c.new('bad1', 'B', 'ZZZZ', 1, 1)
    c.new('bad2', 'B', 'AAPL', 0, 1)
    c.cancel('nope')
    c.new('dup', 'S', 'AAPL', 10, 10050); c.new('dup', 'S', 'AAPL', 10, 10050)
    c.new('fok', 'B', 'AAPL', 1_000_000, 10060, 'FOK')
    show(c)
    print('-> out-of-sequence message must get a Logout')
    c.out_seq += 5
    c.new('late', 'B', 'AAPL', 1, 9000)
    show(c)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1'); ap.add_argument('--port', type=int, default=30000)
    ap.add_argument('cmd', nargs='*', default=['demo'])
    a = ap.parse_args()
    c = Client(a.host, a.port)
    if a.cmd[0] == 'demo':
        demo(c)
    elif a.cmd[0] == 'order':
        side, sym, qty, px = a.cmd[1], a.cmd[2], int(a.cmd[3]), int(a.cmd[4])
        tif = a.cmd[5] if len(a.cmd) > 5 else 'GTC'
        c.login(); show(c)
        c.new(f'o{int(time.time())%100000}', side, sym, qty, px, tif); show(c, 1.0)
    else:
        print(__doc__); sys.exit(2)

if __name__ == '__main__':
    main()
