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
make up         # start the exchange as a background service (keeps running)
make tui        # attach the terminal; q to leave, run again any time
make down       # stop the exchange
```

The exchange is a long-lived process. `make up` starts it detached with a
pid file and a log under `~/.local/state/mktsim/`; by default it runs the
built-in demo order flow in-process (the `flowgen` ingress adapter in the
config, a calm, market-like mix across all configured symbols), so it is a
live market from the first second. Set that adapter's `enabled` to false
to drive it with your own ingress instead. `make status` and `make logs`
show what it's doing; `make tui` attaches `feedviz`, which loads a
snapshot of every book on join, so it doesn't matter when you open it or
how many times. `make demo` does `up` then `tui` in one go.

In the TUI: `T` themes, `/` find a symbol, `e` the per-message view,
`Enter` on a row inspects one message to the byte, `q` quits.

By hand, from the repo root:

```
build/gateway/exchange_server configs/default.json --daemon     # or without --daemon, in the foreground
build/gateway/exchange_server --status | --stop
clients/feedviz/target/release/feedviz --iface 127.0.0.1         # --iface only needed on macOS
build/gateway/flowgen --profile busy                             # extra flow over BOE/TCP, e.g. for load
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
- **Clients recover.** A gap is filled from the TCP retransmit server while
  live packets are held back; a late joiner or an unrecoverable gap loads
  the books from the snapshot server and gates older messages by engine
  sequence. feedviz heals 1,000 forced gaps in four seconds at full rate.
- **Ingress is an API, not a protocol.** Adapters are shared libraries
  against one C header, loaded from config; BOE is just the first one.
- **Measured** on one laptop over loopback: at a calm 2k orders/s across
  100 symbols on 4 shards, ack round trip p50 108 µs, p99 175 µs; 4 BOE
  sessions at 20k orders/s, p50 203 µs, p99 265 µs; 8 sessions at 60k/s,
  p50 169 µs, p99 1.1 ms; feed 70k msgs/s with zero gaps. See
  `gateway/README.md`.

## Status

Working: engine, sharding, feed with retransmission and snapshot servers,
BOE and JSON-lines ingress, the ingress plugin API, flowgen, feedviz with
all three screens and gap recovery, captures and replay.

Not yet: the agent environment and ABIDES bridge; a Python client SDK;
Linux has only been exercised in CI.

If you installed the `mktsim` CLI before the `engine/` rename, rerun
`make install` from `engine/` to refresh the symlink.

## Releases

Tagging `vX.Y.Z` builds release archives: `feedviz` for macOS (arm64 and
x86_64) and Linux x86_64, and the exchange binaries with the default
config and the Python clients for macOS and Linux. Watching a feed then
needs no toolchain: download `feedviz`, run it with the exchange's
address.

## License

MIT.
