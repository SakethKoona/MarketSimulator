# feedviz

Rust terminal UI for the MarketSimulator market data feed. Subscribes to
the UDP multicast feed (or replays a capture), rebuilds every symbol's L3
book, and draws it at 60 fps on three screens: MARKET (ladder, candles
with VWAP, indicators, flow, symbol monitor, tape) at human speed, EVENTS
(every message, filterable, down to the byte), and INSPECT (one message in
full: fields, hex, lifecycle, book impact, packet, timing).

## Layout

```
crates/protocol      zerocopy wire structs for MoldUDP64 and the feed messages,
                     capture-file reader. Mirrors docs/protocol/feed-v1.md.
crates/feed_client   ingest thread (socket or capture) → rtrb ring → book thread
                     (Mold sequencing, gap/duplicate detection, L3 books) →
                     triple-buffered Snapshot for the UI.
crates/tui           the `feedviz` binary: ratatui screens (ui.rs, inspect.rs),
                     app state and keys (app.rs), field decoder (decode.rs),
                     plus a --headless monitor.
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

Live, against an exchange started from the repo root:

```
./gateway/build/exchange_server configs/default.json --rate 20000
./target/release/feedviz --iface 127.0.0.1          # another terminal
```

On macOS use `--iface 127.0.0.1` together with `"interface": "127.0.0.1"`
in the config's `feed` block; on Linux the defaults work. Replay a capture
instead with `--replay testdata/capture.bin` (the replay runs as fast as the
book thread can apply it; pause with space to look around). Other flags:
`--group`, `--port`, `--fps N` (default 60), `--seconds S` to exit after S
seconds, `--headless` for the line-per-second monitor instead of the UI.

Use a GPU terminal (Ghostty, kitty, WezTerm, iTerm2) with a monospace font
that has the block and box-drawing glyphs; 200×55 or larger shows every
column, 140×40 is the comfortable minimum.

### Keys

| key | everywhere |
|---|---|
| `q` | quit |
| `m` / `e` / Tab | MARKET screen / EVENTS screen / toggle |
| `space` | pause the view (the book keeps updating underneath) |
| `[` `]` | previous / next symbol |
| `d` | ladder depth 10 → 14 → 20 → 40 |
| `i` | candle interval 1s → 5s → 30s → 1m |
| `Esc` | clear selection and follow, back to live |

| key | EVENTS screen |
|---|---|
| `1`–`5` | toggle message types A E X D U in the log |
| `f` | log shows current symbol only / all symbols |
| `o` | follow: pin the log to the selected row's order id (again to release) |
| `↑` `↓` / `j` `k`, PgUp, PgDn | select a row (pauses the view) |
| `Enter` | open the inspector on the selected row |
| `n` `p` | next / previous event for the same order |
| `K` | jump to the first event of this packet |

| key | inspector |
|---|---|
| `Esc` / `q` | back to the main screen |
| `↑` `↓` | previous / next event in the log |
| `n` `p` | next / previous event for the same order |
| `K` | jump to the first event of this packet |

### MARKET screen

- **L2 BOOK**: asks above, bids below, price in the centre, cumulative
  depth bars behind the quantities, order count per level, mid, spread and
  imbalance, last trade, volume, live orders and level counts.
- **CANDLES / VOL**: one candle per interval from execution prices (blue
  up, orange down), half-block resolution, a dashed VWAP line, executed
  volume underneath, the forming candle's OHLC and VWAP in the title.
- **INDICATORS**: ten tiles with value, change and a 60-second sparkline:
  last and change since open, VWAP, session high/low, volume, trades and
  trades/s, average trade size, buy volume share (aggressor side), average
  spread, book pressure over the shown levels, and order flow per second.
- **FLOW**: trades per second, buy versus sell executed volume stacked,
  and spread, each over the last 90 seconds.
- **SYMBOL MONITOR**: one row per symbol with last, change, best bid and
  ask, spread, volume, messages per second and a trend sparkline.
- **TIME & SALES** and a three-line **FEED HEALTH** strip.

### EVENTS screen

- **EVENT LOG**: one row per message in stream order: Mold seq, engine time
  to the ns, type, symbol, order id, side, price, qty, remaining, match id,
  book_seq and receive latency; a gap is a red row. The filter bar shows
  the active type toggles, symbol filter and follow target. Columns drop
  from the right as the terminal narrows.
- **INSPECTOR**: the selected message decoded field by field with its raw
  bytes colour-keyed, and the level before and after it.
- **ORDER**: the selected order's lifecycle as a chain, from the events in
  the window.
- **FEED HEALTH**: msgs/s, latency p99 and UI frame-time sparklines, the
  message-type mix, ring occupancy, ingest drops, gaps, duplicates.

### INSPECT screen

Enter on a log row: decoded fields with offsets and meanings, raw bytes,
the order's lifecycle, book impact before and after, both sides of the
match, the Mold packet it arrived in, timing and the spec paragraph.

### Headless

`--headless` prints one block per `--interval-ms`: a stats line, then per
symbol the best bid and ask, spread, last trade, volume, trades, orders,
level counts and the top five levels. `seq` is `shard:sequence`.

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
