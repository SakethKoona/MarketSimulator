# MarketSimulator

A market exchange simulator built the way real venues are: a sharded C++
matching engine, a public market-data feed in ITCH style over UDP
multicast, order entry over a BOE-style binary TCP protocol, a pluggable
ingress API so any system can submit orders, and a Rust terminal client
that rebuilds the books from the feed and shows them Bloomberg-style.

```
                      ┌──────────────────────────── exchange_server ────────────────────────────┐
  BOE / TCP ───────▶  │ ingress adapters ─▶ order-entry core ─▶ shard engines (L3 books) ──┐     │
  JSON lines / TCP ─▶ │   (built-in BOE, loadable plugins)        ▲     │                 │     │
  your adapter ────▶  │                                          reports ◀────────────────┘     │
                      │                                                   │ events                 │
                      │                                      feed publisher ─▶ MoldUDP64 packets │
                      └───────────────────────────────────────────────────┼──────────────────────┘
                                                                          ▼  UDP multicast 239.1.1.1:30001
                                                  feedviz (TUI) · feed_dump · your client
```

## Quick start

Requirements: a C++20 compiler, CMake 3.20+, Rust stable, tmux (optional).

```
git clone https://github.com/SakethKoona/MarketSimulator && cd MarketSimulator
make            # builds engine, gateway, plugins (CMake) and feedviz (cargo)
make test       # engine suite, gateway suite incl. feed reconstruction, Rust tests
make demo       # exchange + order flow + TUI in a tmux window
```

`make demo` opens three panes: the exchange, `flowgen` sending orders over
BOE at a calm rate across the configured symbols, and `feedviz` listening
on the multicast feed. Press `T` for themes, `/` to find a symbol, `e` for
the per-message view, `Enter` on a row to inspect one message to the byte,
`q` to quit. Without tmux the same runs with feedviz in front.

By hand, from the repo root:

```
build/gateway/exchange_server configs/default.json
build/gateway/flowgen --profile calm                       # or --rate 20000 --sessions 8
clients/feedviz/target/release/feedviz --iface 127.0.0.1   # --iface only needed on macOS
```

Place an order yourself with `python3 gateway/tools/boe_client.py demo`, or
`python3 gateway/tools/jsonl_client.py demo` through the JSON-lines plugin.

## What's here

| directory | what | docs |
|---|---|---|
| `engine/` | matching engine: L3 order books on skiplists with arena pools, price-time priority, GTC/IOC/FOK, cancel-replace, one engine and event sink per shard, the `mktsim` CLI and a bench | `engine/cli/README.md` |
| `gateway/` | feed publisher (ITCH-style messages framed MoldUDP64 over multicast), the ingress core and BOE gateway, the JSON-lines ingress plugin, `flowgen`, `feed_dump` | `gateway/README.md` |
| `clients/feedviz/` | Rust TUI: ingest → lock-free ring → book thread → snapshot → 60 fps ratatui; MARKET, EVENTS and INSPECT screens, 24 themes, fuzzy pickers | `clients/feedviz/README.md` |
| `docs/protocol/` | the wire contracts: `feed-v1.md`, `boe-v1.md` | |
| `docs/ingress-api.md` | the C ingress API for writing your own order-entry adapter | |
| `configs/` | symbols, shards, feed and ingress settings | |

## Properties worth knowing

- **The feed is proven, not assumed.** `feed_reconstruct_test` drives the
  engine with 200k random operations, encodes the events exactly as the
  publisher does, rebuilds an L3 book from the bytes, and asserts it equals
  the engine's book level by level and in time-priority order. feedviz's
  golden test does the same from a recorded capture in Rust.
- **Nothing blocks the engine.** Events leave each shard through a lock-free
  SPSC ring that drops and counts rather than stalls; orders arrive through
  per-shard lock-free MPMC queues; the publisher skips sequence numbers on
  overflow so clients see a gap instead of a wrong book.
- **Ingress is an API, not a protocol.** Adapters are shared libraries
  against one C header, loaded from config; BOE is just the first one.
- **Measured** on one laptop over loopback, 3 shards: 4 BOE sessions at
  20k orders/s, ack round trip p50 203 µs, p99 265 µs; 8 sessions at 60k/s,
  p50 169 µs, p99 1.1 ms; feed 70k msgs/s with zero gaps. See
  `gateway/README.md`.

## Status

Working: engine, sharding, feed, BOE and JSON-lines ingress, the ingress
plugin API, flowgen, feedviz with all three screens, captures and replay.

Not yet: feed retransmission and snapshot servers (a client that misses
packets or joins late reconstructs forward only), realistic order-flow
profiles beyond rate presets, the agent environment and ABIDES bridge.

If you installed the `mktsim` CLI before the `engine/` rename, rerun
`make install` from `engine/` to refresh the symlink.

## License

MIT.
