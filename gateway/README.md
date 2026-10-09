# gateway

Ingress (BOE over TCP) and egress (ITCH-style feed over UDP multicast) for
the matching engine in `../engine`. The wire contracts live in
`../docs/protocol/`; this directory implements them.

## Build and test

```
make            # builds build/exchange_server, build/feed_dump, runs tests
make test       # protocol framing, SPSC ring, feed reconstruction conformance
```

Compiles the engine sources directly, so the engine's own Makefile is not
needed. Requires the nlohmann JSON headers in `../engine/third_party`.

## Run

```
# from the repo root
build/gateway/exchange_server configs/default.json          # engine + feed + BOE, no flow
build/gateway/flowgen --sessions 4 --rate 20000              # order flow over BOE/TCP
build/gateway/feed_dump                                      # watch the feed
```

`exchange_server` runs the engine, the feed publisher and the BOE gateway.
Order flow comes in over BOE; `flowgen` is the external generator. Flags:
`--rate N` turns on the old in-process synthetic generator (default 0),
`--seconds S` stops automatically, `--quiet` suppresses the per-second
stats, `--capture FILE` appends every sent packet to FILE (u32 little-endian
length + raw MoldUDP64 packet) and, at shutdown, writes `FILE.books.txt`
with the final levels (`symbol_id side price qty count`). Captures are the
replay and golden-test input for `clients/feedviz`.

`flowgen` opens N BOE sessions, each on its own thread, and drives the
order mix (55% passive limits around a random-walk mid anchored to its own
fills, 25% cancels, 10% quantity reductions, 10% crossing IOCs) at an
aggregate `--rate`, tracking every order through acknowledgments,
executions, modifies and cancels. Flags: `--host`, `--port`, `--sessions`,
`--rate`, `--seconds`, `--symbols AAPL,GOOG,NVDA`, `--seed`, `--quiet`. It
prints one line per second: sent, acked, rejected, busy (gateway ring
full), executions, cancels, modifies, live orders, and ack round-trip
latency p50/p99/max. Measured on one laptop over loopback with 3 shards:

| sessions | rate | rtt p50 | rtt p99 | feed |
|---:|---:|---:|---:|---|
| 4 | 20k/s | 214 µs | 294 µs | 23k msgs/s, 0 gaps |
| 8 | 60k/s | 169 µs | 1.1 ms | 70k msgs/s, 0 gaps |

The BOE gateway is a single poll thread; beyond this, scale by running one
gateway thread per group of sessions (not yet done).

`feed_dump [group] [port] [iface]` joins the multicast group and prints every
decoded message, heartbeats, gaps and duplicate packets.

### macOS note

Sending on the default interface to a listener on the same host delivers
each packet twice. For same-host testing set `"interface": "127.0.0.1"` in
the config's `feed` block and pass `127.0.0.1` as `feed_dump`'s third
argument. Clients must drop duplicates by sequence number regardless.

## Feed recovery

Two TCP services sit beside the multicast feed (ports in the config's
`feed` block): the **retransmit server** (default 30002) answers a
`{session, seq, count}` request with the messages re-framed as Mold
packets from the publisher's store (65,536 messages by default), and the
**snapshot server** (default 30003) sends every resting order for one or
all symbols as Add Orders between Snapshot Start/End messages, as of the
shard's current `book_seq`, built on the shard thread between commands.
Both are specified in `docs/protocol/feed-v1.md` §2.1 and §5.

## Ingress adapters

Order entry is pluggable. The exchange exposes a C API
([`include/ingress/api.h`](include/ingress/api.h), documented in
[`docs/ingress-api.md`](../docs/ingress-api.md)); anything that speaks it
can be an ingress: the built-in BOE gateway, the JSON-lines adapter in
`plugins/jsonl_ingress.cpp` (built as a shared library and loaded from the
config), or your own. The config's `ingress` list says what runs:

```json
"ingress": [
  { "type": "boe", "port": 30000 },
  { "type": "plugin", "path": "gateway/build/jsonl_ingress.dylib", "config": { "port": 30020 } }
]
```

Behind the API, `src/ingress/core.cpp` owns sessions, one lock-free
multi-producer command queue per shard, one report queue per session, and
order-to-session routing on the shard engine threads. Each adapter runs on
its own threads, so adding adapters adds ingress capacity.

Try the JSON-lines adapter: `python3 tools/jsonl_client.py demo`.

## Layout

```
include/protocol/   feed.hpp, moldudp64.hpp, boe.hpp   packed wire structs
include/ingress/    api.h (the C ingress API), core.hpp, plugin.hpp
include/            udp_multicast.hpp, feed_encoder.hpp, feed_framer.hpp,
                    feed_publisher.hpp, boe_server.hpp
src/                feed_publisher.cpp, boe_server.cpp, exchange_server.cpp
src/ingress/        core.cpp (sessions, queues, routing), plugin.cpp (dlopen)
plugins/            jsonl_ingress.cpp, the worked example adapter
tools/              feed_dump.cpp, flowgen.cpp, boe_client.py, jsonl_client.py
tests/              protocol_test, spsc_ring_test, feed_reconstruct_test
```

The lock-free SPSC ring the engine's `EventSink` uses is
`../engine/include/spsc_ring.hpp`.

## Threads

- **engine thread** (main): matching engine, synthetic flow, and the BOE
  command loop.
- **publisher thread**: drains the event sink, pairs Execute + TradeFill
  into one Order Executed, frames MoldUDP64 packets, multicasts, heartbeats,
  repeats the Stock Directory.
- **adapter threads**: each ingress adapter runs its own (the BOE gateway
  is one poll thread); they submit into per-shard lock-free queues and poll
  per-session report queues through the ingress API.
