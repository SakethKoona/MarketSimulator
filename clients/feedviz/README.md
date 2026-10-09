# feedviz

Rust client for the MarketSimulator market data feed. Subscribes to the
UDP multicast feed (or replays a capture), rebuilds every symbol's L3 book,
and hands the UI a snapshot a few dozen times a second. The terminal UI
from the design canvas sits on top of this; checkpoint 1 is the pipeline
plus a headless monitor you can run and test.

## Layout

```
crates/protocol      zerocopy wire structs for MoldUDP64 and the feed messages,
                     capture-file reader. Mirrors docs/protocol/feed-v1.md.
crates/feed_client   ingest thread (socket or capture) → rtrb ring → book thread
                     (Mold sequencing, gap/duplicate detection, L3 books) →
                     triple-buffered Snapshot for the UI.
crates/tui           the `feedviz` binary. Headless monitor for now.
testdata/            capture.bin (+ .books.txt): a 3-shard recording from
                     exchange_server used by the golden test.
```

## Build and test

```
cd clients/feedviz
cargo build --release
cargo test --release
```

The golden test replays `testdata/capture.bin` and requires the rebuilt
books to equal the engine's final books, level by level (quantity and
order count), with no gaps, no unknown orders and no out-of-order book
sequence. A second fixture, `capture_1shard.bin`, is checked when present.

## Run

Replay a capture as fast as the book thread can take it:

```
./target/release/feedviz --replay testdata/capture.bin --depth 5
```

Listen to a live exchange. Start the server first from the repo root:

```
./gateway/build/exchange_server configs/default.json --rate 20000
```

then in another terminal:

```
./target/release/feedviz --iface 127.0.0.1 --depth 5
```

On macOS use `--iface 127.0.0.1` together with `"interface": "127.0.0.1"`
in the config's `feed` block; on Linux the defaults work. Other flags:
`--group`, `--port`, `--seconds S` to exit after S seconds,
`--interval-ms` for the print cadence.

The monitor prints one block per interval: a stats line (Mold sequence,
message rate, packets, heartbeats, gaps, lost messages, duplicates, ring
occupancy, ingest drops, receive latency p50/p99, STALE after a gap), then
per symbol the best bid and ask, spread, last trade with aggressor arrow,
volume, trade count, live order count, level counts, and the top N levels
as `count qty price | price qty count`. `seq` is shown as `shard:sequence`.

## What to expect

- **Joining mid-session** shows tickers as `#id` until the next Stock
  Directory (every 2 seconds) and counts `unknown=` for messages about
  orders added before you joined. Both are expected until the snapshot
  server exists; the books are correct from that point forward.
- **Replaying a capture** reports latency as 0 (no receive timestamps) and
  ends with `ENDED` once the End of Session packet is applied.
- **Gaps** set STALE and are counted; recovery via retransmission is the
  next exchange-side feature.

## Record a new fixture

```
./gateway/build/exchange_server configs/default.json --rate 3000 --seconds 3 \
    --quiet --capture clients/feedviz/testdata/capture.bin
```

writes the packets and `capture.bin.books.txt`; the golden test picks both
up by name.
