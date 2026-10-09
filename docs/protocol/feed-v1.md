# MarketSimulator Market Data Feed, version 1

This document is the contract for the public market data feed. Anyone with a
UDP socket and this document can build a client. The exchange's C++ encoder
and the project's Rust TUI are two implementations of it; neither is the
source of truth, this file is.

## 1. Conventions

- **Byte order:** little-endian everywhere, including the MoldUDP64 header.
  This deviates from Nasdaq ITCH and MoldUDP64, which are big-endian.
- **Integers:** fixed width, unsigned, no padding. Structs are packed.
- **Characters:** ASCII, left-justified, space-padded, not NUL-terminated.
- **Prices:** `u64`, in integer price units. Version 1 uses whole units with
  no implied decimals. The `price_scale` field of the Stock Directory message
  tells the client how many implied decimals a symbol uses, so a later
  version can switch to fixed-point without a wire change.
- **Quantities:** `u32` shares.
- **Timestamps:** `u64` nanoseconds since the Unix epoch, wall clock.
- **Identifiers:** `order_id`, `match_id`, `book_seq` are `u64`.
  `symbol_id` is `u32`. The value 0 is never a valid `order_id`.
- **Sides:** `u8`, ASCII `'B'` for buy, `'S'` for sell.

## 2. Transport: MoldUDP64 over UDP multicast

Default group `239.1.1.1`, port `30001`. Override through exchange config.

Every UDP datagram is one MoldUDP64 packet:

| offset | size | type     | field           | notes                                   |
|-------:|-----:|----------|-----------------|-----------------------------------------|
| 0      | 10   | char[10] | session         | identifies the trading session, e.g. `20261008A ` |
| 10     | 8    | u64      | sequence_number | sequence of the first message in this packet |
| 18     | 2    | u16      | message_count   | number of message blocks that follow     |
| 20     | ...  |          | message blocks  | `message_count` blocks                   |

Each message block:

| size | type | field          |
|-----:|------|----------------|
| 2    | u16  | message_length | bytes of payload that follow |
| n    |      | payload        | one feed message, section 3   |

Rules:

- `sequence_number` increases by one per **message**, not per packet. A
  packet carrying messages 100..104 has `sequence_number = 100` and
  `message_count = 5`. The next packet starts at 105.
- A **heartbeat** is a packet with `message_count = 0` and the next expected
  sequence number. The exchange sends one at least every 1 second when
  idle.
- **End of session** is a packet with `message_count = 0xFFFF`.
- A packet is at most 1400 bytes so it fits in one Ethernet frame.
- The first sequence number of a session is 1.
- **Duplicates are possible.** Some kernels deliver a locally sent multicast
  packet to a same-host listener more than once (macOS does this via en0).
  A client must ignore any packet whose `sequence_number` is below the next
  expected one. This is the same rule that makes A/B feed arbitration work.
- **Engine overflow.** If the exchange's internal event queue overflows, the
  lost events are never published; the stream's `sequence_number` skips by
  the number lost so clients detect a gap. Such a gap cannot be filled by
  retransmission (section 2.1 replies with its oldest sequence), and the
  client must resynchronise from a snapshot once version 2 provides one.

### 2.1 Gap detection and retransmission

A client tracks the next expected sequence number. If a packet arrives with
a higher one, messages were lost. The client requests them from the
retransmission server over TCP (default port `30002`):

Request, 20 bytes:

| size | type     | field           |
|-----:|----------|-----------------|
| 10   | char[10] | session         |
| 8    | u64      | sequence_number | first message wanted |
| 2    | u16      | message_count   | how many, at most 1000 per request |

The server replies with one or more MoldUDP64 packets in the same framing as
the multicast stream, then closes the connection. If the requested range is
older than the server's buffer, it replies with a packet whose
`message_count = 0` and whose `sequence_number` is the oldest it still has;
the client must then resynchronise from a snapshot (section 5). A request
for a sequence not yet published is answered the same way, with the next
sequence to be published.

**Recovering from a gap.** On detecting a gap the client should buffer live
packets, request the missing range, apply the retransmitted messages, then
drain the buffer in order. Messages are idempotent by sequence: a client
that receives a sequence it has already applied drops it.

## 3. Messages

The first byte of every payload is the message type. Offsets below are
relative to the start of the payload.

### 3.1 System Event, type `'S'`, 12 bytes

| offset | size | type | field      | values |
|-------:|-----:|------|------------|--------|
| 0      | 1    | u8   | type       | `'S'`  |
| 1      | 1    | u8   | event_code | `'O'` start of session, `'C'` end of session |
| 2      | 2    | u16  | version    | protocol version, `1` |
| 4      | 8    | u64  | ts_ns      | |

A client that receives a `version` it does not understand must stop and
report the mismatch rather than keep decoding.

### 3.2 Stock Directory, type `'R'`, 24 bytes

| offset | size | type    | field       | notes |
|-------:|-----:|---------|-------------|-------|
| 0      | 1    | u8      | type        | `'R'` |
| 1      | 1    | u8      | price_scale | implied decimals in prices for this symbol, `0` in v1 |
| 2      | 2    | u16     | reserved    | `0` |
| 4      | 4    | u32     | symbol_id   | |
| 8      | 8    | char[8] | ticker      | e.g. `AAPL    ` |
| 16     | 8    | u64     | ts_ns       | |

One per symbol, sent at the start of the session and repeated every
`directory_interval` seconds (default 5) so late joiners can decode ids.

### 3.3 Add Order, type `'A'`, 42 bytes

A new order is resting on the book.

| offset | size | type | field     |
|-------:|-----:|------|-----------|
| 0      | 1    | u8   | type `'A'` |
| 1      | 1    | u8   | side      |
| 2      | 4    | u32  | symbol_id |
| 6      | 8    | u64  | order_id  |
| 14     | 8    | u64  | price     |
| 22     | 4    | u32  | qty       |
| 26     | 8    | u64  | book_seq  |
| 34     | 8    | u64  | ts_ns     |

### 3.4 Order Executed, type `'E'`, 54 bytes

A resting order was matched, in part or in full. The aggressor is the
opposite side of `side`. When `remaining_qty` is 0 the order has left the
book; no separate Delete follows.

| offset | size | type | field         |
|-------:|-----:|------|---------------|
| 0      | 1    | u8   | type `'E'`    |
| 1      | 1    | u8   | side          | side of the resting order |
| 2      | 4    | u32  | symbol_id     |
| 6      | 8    | u64  | order_id      | the resting order |
| 14     | 8    | u64  | price         | execution price, equals the resting price |
| 22     | 4    | u32  | exec_qty      |
| 26     | 4    | u32  | remaining_qty |
| 30     | 8    | u64  | match_id      | one per fill, shared by nothing else |
| 38     | 8    | u64  | book_seq      |
| 46     | 8    | u64  | ts_ns         |

Trade volume is the sum of `exec_qty` over Order Executed messages. There
is **no** separate Trade message in version 1: every fill in a central limit
order book has a resting counterparty, so Order Executed covers all volume.
Type `'P'` is reserved for a future hidden-liquidity extension.

### 3.5 Order Cancel, type `'X'`, 33 bytes

A resting order's quantity was reduced by its owner. Identity and priority
are unchanged.

| offset | size | type | field         |
|-------:|-----:|------|---------------|
| 0      | 1    | u8   | type `'X'`    |
| 1      | 4    | u32  | symbol_id     |
| 5      | 8    | u64  | order_id      |
| 13     | 4    | u32  | remaining_qty | new resting quantity, always > 0 |
| 17     | 8    | u64  | book_seq      |
| 25     | 8    | u64  | ts_ns         |

### 3.6 Order Delete, type `'D'`, 29 bytes

A resting order was removed by its owner, or a replace left nothing resting.

| offset | size | type | field      |
|-------:|-----:|------|------------|
| 0      | 1    | u8   | type `'D'` |
| 1      | 4    | u32  | symbol_id  |
| 5      | 8    | u64  | order_id   |
| 13     | 8    | u64  | book_seq   |
| 21     | 8    | u64  | ts_ns      |

### 3.7 Order Replace, type `'U'`, 42 bytes

A resting order changed price and/or increased quantity. It keeps its
`order_id` but **loses time priority**: the client must remove it from its
current level and append it at the back of the new one. Any fills caused by
the replacement are published as Order Executed messages *before* this
message, with the same `order_id` as the aggressor.

| offset | size | type | field      |
|-------:|-----:|------|------------|
| 0      | 1    | u8   | type `'U'` |
| 1      | 1    | u8   | side       |
| 2      | 4    | u32  | symbol_id  |
| 6      | 8    | u64  | order_id   |
| 14     | 8    | u64  | price      | new price |
| 22     | 4    | u32  | qty        | new resting quantity |
| 26     | 8    | u64  | book_seq   |
| 34     | 8    | u64  | ts_ns      |

## 4. Sequencing semantics

Two sequence spaces exist and must not be confused:

- **MoldUDP64 `sequence_number`** is transport level. It counts messages on
  the stream and is what gap detection uses.
- **`book_seq`** is the matching engine's sequence. The engine is sharded
  by symbol, so `book_seq` is strictly increasing **within a symbol** and
  must not be compared across symbols. It is unique across the whole feed.
  Several messages can share one `book_seq` when they describe the same
  engine action (today none do on the wire, but clients must tolerate it).
- **`order_id` and `match_id`** are unique across the whole feed. Their top
  8 bits identify the engine shard; clients should treat them as opaque.

Within one symbol, applying messages in stream order reproduces the book
exactly. Across symbols the stream interleaves shards in no defined order.

## 5. Session bootstrap and snapshots

At session start the exchange publishes: System Event `'O'`, then one Stock
Directory per symbol, then order flow. A client that joins later, or that
falls too far behind for retransmission, loads the books from the snapshot
server (default TCP port `30003`) and then applies the live stream.

Request, 14 bytes:

| size | type     | field     |
|-----:|----------|-----------|
| 10   | char[10] | session   |
| 4    | u32      | symbol_id | `0xFFFFFFFF` for every symbol |

The server replies with MoldUDP64 packets whose `sequence_number` is 0
(snapshot packets carry no stream position), then closes the connection.
For each symbol the packets contain, in order:

### 5.1 Snapshot Start, type `'Q'`, 25 bytes

| offset | size | type | field       |
|-------:|-----:|------|-------------|
| 0      | 1    | u8   | type `'Q'`  |
| 1      | 4    | u32  | symbol_id   |
| 5      | 8    | u64  | book_seq    | the book is as of this engine sequence |
| 13     | 4    | u32  | order_count | Add Order messages that follow |
| 17     | 8    | u64  | ts_ns       |

### 5.2 One Add Order (§3.3) per resting order

In price-time priority order, best level first, oldest order first within a
level, each with `book_seq` equal to the Snapshot Start's.

### 5.3 Snapshot End, type `'Z'`, 17 bytes

| offset | size | type | field       |
|-------:|-----:|------|-------------|
| 0      | 1    | u8   | type `'Z'`  |
| 1      | 4    | u32  | symbol_id   |
| 5      | 4    | u32  | order_count |
| 9      | 8    | u64  | ts_ns       |

**Applying a snapshot.** Replace the symbol's book with the Add Orders, then
resume the live stream and, for that symbol, ignore any message whose
`book_seq` is less than or equal to the Snapshot Start's `book_seq`; apply
everything above it. Because `book_seq` is strictly increasing within a
shard, this is exact regardless of when the snapshot was taken relative to
the multicast stream. A client should buffer live packets while a snapshot
is loading rather than apply them to the old book.

## 6. Reference encodings

Hex of an Add Order, little-endian: buy, symbol 0, order 7, price 105,
qty 5, book_seq 2, ts 0:

```
41 42 00000000 0700000000000000 6900000000000000 05000000 0200000000000000 0000000000000000
```
