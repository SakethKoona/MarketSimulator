# gateway

Ingress (BOE over TCP) and egress (ITCH-style feed over UDP multicast) for
the matching engine in `../limit_order_book`. The wire contracts live in
`../docs/protocol/`; this directory implements them.

## Build and test

```
make            # builds build/exchange_server, build/feed_dump, runs tests
make test       # protocol framing, SPSC ring, feed reconstruction conformance
```

Compiles the engine sources directly, so the engine's own Makefile is not
needed. Requires the nlohmann JSON headers in `../limit_order_book/third_party`.

## Run

```
# from the repo root
./gateway/build/exchange_server configs/default.json --rate 2000
./gateway/build/feed_dump                       # another terminal
```

`exchange_server` runs the engine with a synthetic order generator and the
feed publisher. Flags: `--rate N` orders per second, `--seconds S` to stop
automatically, `--quiet` to suppress the one-line-per-second stats.

`feed_dump [group] [port] [iface]` joins the multicast group and prints every
decoded message, heartbeats, gaps and duplicate packets.

### macOS note

Sending on the default interface to a listener on the same host delivers
each packet twice. For same-host testing set `"interface": "127.0.0.1"` in
the config's `feed` block and pass `127.0.0.1` as `feed_dump`'s third
argument. Clients must drop duplicates by sequence number regardless.

## Layout

```
include/protocol/   feed.hpp, moldudp64.hpp, boe.hpp   packed wire structs
include/            udp_multicast.hpp, feed_encoder.hpp, feed_framer.hpp,
                    feed_publisher.hpp
src/                feed_publisher.cpp, exchange_server.cpp
tools/              feed_dump.cpp
tests/              protocol_test, spsc_ring_test, feed_reconstruct_test
```

The lock-free SPSC ring the engine's `EventSink` uses is
`../limit_order_book/include/spsc_ring.hpp`.

## Threads

- **engine thread** (main): matching engine, synthetic flow, and the BOE
  command loop.
- **publisher thread**: drains the event sink, pairs Execute + TradeFill
  into one Order Executed, frames MoldUDP64 packets, multicasts, heartbeats,
  repeats the Stock Directory.
- **gateway thread**: BOE TCP sessions; decodes into a command ring toward
  the engine, encodes execution reports from a report ring back to sessions.
