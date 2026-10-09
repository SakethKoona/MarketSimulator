# MarketSimulator Binary Order Entry (BOE), version 1

Order entry protocol over TCP. Modelled on Cboe BOE: a session layer with
login, heartbeats and per-direction sequence numbers, and fixed-size binary
application messages. Default port `30000`.

## 1. Conventions

Same as the feed spec: little-endian, packed structs, ASCII space-padded
text, prices `u64` in integer units, quantities `u32`, timestamps `u64`
nanoseconds since the Unix epoch. `cl_ord_id` is a 20-byte client-chosen
identifier that must be unique per session for the life of the order.

## 2. Framing

Every message starts with a 10-byte header:

| offset | size | type | field            | notes |
|-------:|-----:|------|------------------|-------|
| 0      | 2    | u16  | start_of_message | always `0xBABA` |
| 2      | 2    | u16  | message_length   | bytes after this field, header remainder plus body |
| 4      | 1    | u8   | message_type     | section 3 |
| 5      | 1    | u8   | matching_unit    | `0` in v1 |
| 6      | 4    | u32  | sequence_number  | per direction, see 2.1 |

A receiver reads 4 bytes, validates the marker, then reads
`message_length` more bytes. An unknown `message_type` of a known length is
skipped; a bad marker closes the connection.

### 2.1 Sequencing

Each direction has its own counter starting at 1 for the first application
message after login. Session messages (login, logout, heartbeats) carry
`sequence_number = 0`. The server rejects a client application message whose
sequence number is not exactly the next expected one and closes the session.

### 2.2 Heartbeats

If either side has sent nothing for 1 second it sends a heartbeat. If either
side has received nothing for 5 seconds it closes the connection. Closing
the socket does **not** cancel resting orders in v1; cancel-on-disconnect is
a v2 option.

## 3. Messages

### 3.1 Session, client to server

| type   | name            | body |
|--------|-----------------|------|
| `0x01` | LoginRequest    | `session_sub_id` char[4], `username` char[4], `password` char[10] |
| `0x02` | LogoutRequest   | none |
| `0x03` | ClientHeartbeat | none |

### 3.2 Session, server to client

| type   | name             | body |
|--------|------------------|------|
| `0x07` | LoginResponse    | `status` u8, `last_received_seq` u32, `text` char[60] |
| `0x08` | Logout           | `reason` u8, `text` char[60] |
| `0x09` | ServerHeartbeat  | none |

`LoginResponse.status`: `'A'` accepted, `'N'` not authorised, `'D'` session
already logged in, `'B'` bad protocol version.

### 3.3 Orders, client to server

**NewOrder, `0x04`, body 43 bytes**

| offset | size | type     | field     | values |
|-------:|-----:|----------|-----------|--------|
| 0      | 20   | char[20] | cl_ord_id | |
| 20     | 1    | u8       | side      | `'B'` / `'S'` |
| 21     | 4    | u32      | qty       | > 0 |
| 25     | 8    | u64      | price     | ignored for market orders |
| 33     | 8    | char[8]  | symbol    | ticker as in the Stock Directory |
| 41     | 1    | u8       | ord_type  | `'1'` market, `'2'` limit |
| 42     | 1    | u8       | tif       | `'0'` GTC, `'3'` IOC, `'4'` FOK |

**CancelOrder, `0x05`, body 20 bytes**

| offset | size | type     | field          |
|-------:|-----:|----------|----------------|
| 0      | 20   | char[20] | orig_cl_ord_id |

**ModifyOrder, `0x06`, body 52 bytes**

| offset | size | type     | field          | notes |
|-------:|-----:|----------|----------------|-------|
| 0      | 20   | char[20] | cl_ord_id      | new id for the order after the modify |
| 20     | 20   | char[20] | orig_cl_ord_id | |
| 40     | 4    | u32      | qty            | new total quantity |
| 44     | 8    | u64      | price          | new price, `0` keeps the current price |

Semantics follow the engine: a quantity decrease at the same price keeps
priority; a price change or quantity increase is a cancel-replace that keeps
the exchange `order_id` but loses priority and may execute immediately.

### 3.4 Execution reports, server to client

All begin with `ts_ns` u64 and `cl_ord_id` char[20].

**OrderAcknowledgment, `0x25`, body 61 bytes**

| offset | size | type     | field      |
|-------:|-----:|----------|------------|
| 0      | 8    | u64      | ts_ns      |
| 8      | 20   | char[20] | cl_ord_id  |
| 28     | 8    | u64      | order_id   | exchange id, matches the public feed |
| 36     | 8    | char[8]  | symbol     |
| 44     | 1    | u8       | side       |
| 45     | 4    | u32      | qty        |
| 49     | 8    | u64      | price      |
| 57     | 4    | u32      | leaves_qty | quantity resting after any immediate fills |

Sent once per accepted NewOrder, **before** any OrderExecution for it. An
IOC that fully fills still gets an Acknowledgment with `leaves_qty = 0`.

**OrderRejected, `0x26`, body 89 bytes**

| offset | size | type     | field     |
|-------:|-----:|----------|-----------|
| 0      | 8    | u64      | ts_ns     |
| 8      | 20   | char[20] | cl_ord_id |
| 28     | 1    | u8       | reason    |
| 29     | 60   | char[60] | text      |

`reason`: `'S'` unknown symbol, `'Q'` bad quantity, `'P'` bad price,
`'K'` fill-or-kill could not fill, `'L'` no liquidity (an IOC or market
order that executed nothing), `'D'` duplicate cl_ord_id, `'U'` unknown
order (for cancel/modify), `'X'` session not logged in, `'O'` other.

**OrderModified, `0x27`, body 52 bytes**

| offset | size | type     | field      |
|-------:|-----:|----------|------------|
| 0      | 8    | u64      | ts_ns      |
| 8      | 20   | char[20] | cl_ord_id  |
| 28     | 8    | u64      | order_id   |
| 36     | 4    | u32      | qty        |
| 40     | 8    | u64      | price      |
| 48     | 4    | u32      | leaves_qty |


**OrderCancelled, `0x28`, body 37 bytes**

| offset | size | type     | field     |
|-------:|-----:|----------|-----------|
| 0      | 8    | u64      | ts_ns     |
| 8      | 20   | char[20] | cl_ord_id |
| 28     | 8    | u64      | order_id  |
| 36     | 1    | u8       | reason    | `'U'` user requested, `'I'` IOC remainder, `'K'` FOK failed, `'R'` replaced away |

**OrderExecution, `0x2C`, body 61 bytes**

| offset | size | type     | field       |
|-------:|-----:|----------|-------------|
| 0      | 8    | u64      | ts_ns       |
| 8      | 20   | char[20] | cl_ord_id   |
| 28     | 8    | u64      | order_id    |
| 36     | 8    | u64      | match_id    | same value as on the public feed |
| 44     | 4    | u32      | last_qty    |
| 48     | 8    | u64      | last_price  |
| 56     | 4    | u32      | leaves_qty  |
| 60     | 1    | u8       | side        |

Sent to the owner of **both** sides of a fill. The aggressor receives its
Acknowledgment first, then one OrderExecution per fill.

## 4. Mapping to the engine

The gateway, not the engine, owns the `cl_ord_id` to `order_id` mapping and
the `order_id` to session mapping. The engine only ever sees `order_id`,
`symbol_id` and numeric fields; symbol lookup happens in the gateway. Private
reports are routed by `order_id`, so the engine does not need to know
sessions.
