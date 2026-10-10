# MarketSimulator ingress API, version 1

The ingress API is how order flow enters the exchange. The built-in BOE
gateway uses it, and so can anything else: a FIX engine, a websocket
bridge, a bot framework, a replay tool. An adapter is a shared library
that the exchange loads from its config; it talks to the exchange only
through the function table in [`gateway/include/ingress/api.h`](../gateway/include/ingress/api.h).

If your system speaks our API, it plugs in. Nothing in the exchange needs
to change, and the exchange never needs to know your protocol.

## The shape of it

```
 your protocol ──▶ your adapter (shared library) ──▶ ingress API ──▶ exchange core ──▶ shard engines
                                                 ◀── reports  ◀──
```

- The exchange owns sessions, order ids, shard routing and report routing.
- The adapter owns everything client-facing: its wire protocol, client
  order ids, authentication, reconnection, whatever its clients expect.
- The boundary is plain C, versioned, little-endian, no exceptions.

## Lifecycle

The exchange looks up four symbols in your library and calls them in order:

| symbol | when | what you do |
|---|---|---|
| `mktsim_ingress_init(api, ex, config_json, &state)` | once at startup | check `api->version`, parse your config object, allocate state. Return 0 or startup aborts. |
| `mktsim_ingress_start(state)` | after every adapter initialised | listen, spawn threads. Return 0. |
| `mktsim_ingress_stop(state)` | at shutdown | stop serving, join threads. |
| `mktsim_ingress_destroy(state)` | after stop | free state. |

`config_json` is your entry's `config` object from the exchange config, as
text. Keep whatever you need from `api` and `ex`; they stay valid until
`destroy` returns.

## Sessions

A session is the unit of ownership: every order submitted on a session
reports back to that session for its whole life, including fills that
happen long after submission. Typically one client connection is one
session.

```c
mktsim_session *s = api->open_session(ex, "my-adapter");   // NULL if the exchange is out of session slots
...
api->close_session(s);
```

Rules:

- `submit` and `poll` on one session may be called from any thread, but
  not concurrently with each other on the same session. Different sessions
  are fully independent, so one thread per connection or one thread for
  many connections both work.
- Closing a session drops any reports still queued for it. Orders it owns
  stay on the book (cancel-on-disconnect is the adapter's choice).

## Submitting

```c
mktsim_order_req r = {0};
r.request_id = 42;              // yours; echoed on the first report
r.kind = MKTSIM_REQ_NEW;
r.side = MKTSIM_BUY;            // 'B' / 'S'
r.ord_type = MKTSIM_LIMIT;      // or MKTSIM_MARKET (then tif must be IOC or FOK)
r.tif = MKTSIM_GTC;             // GTC, IOC, FOK
r.symbol_id = id;               // from api->resolve_symbol(ex, "AAPL", 4, &id)
r.qty = 100;
r.price = 10000;                // integer price units
int rc = api->submit(s, &r);
```

`submit` never blocks. `MKTSIM_OK` means queued; anything else means
nothing was queued: `MKTSIM_EBUSY` (the shard's queue is full, back off
and retry), `MKTSIM_EBADSYMBOL`, `MKTSIM_EBADREQ` (zero qty, bad side or
type, limit at price 0, modify to qty 0: use cancel), `MKTSIM_ECLOSED`.

Cancel and modify use the exchange `order_id` you received in the Accepted
report:

```c
r.kind = MKTSIM_REQ_CANCEL; r.order_id = oid;
r.kind = MKTSIM_REQ_MODIFY; r.order_id = oid; r.qty = 60; r.price = 0;   // price 0 keeps the price
```

A quantity decrease at the same price keeps queue priority. A price change
or quantity increase is a cancel-replace: the order keeps its id, loses
priority, and may execute immediately.

## Reports

Reports are polled, never pushed, so your threads stay yours:

```c
static void on_report(void *user, const mktsim_report *r) { ... }
size_t n = api->poll(s, on_report, user, 1024);
```

| kind | meaning | fields that matter |
|---|---|---|
| `ACCEPTED` | a new order was accepted; it may have filled on arrival | `request_id`, `order_id`, `qty`, `price`, `side`, `symbol_id`, `leaves_qty` (resting qty, 0 if nothing rests) |
| `REJECTED` | a request was refused | `request_id`, `status` (`MKTSIM_ST_*`); for a new order `order_id` is 0 |
| `EXECUTION` | one fill on `order_id` | `order_id`, `last_qty`, `price`, `match_id`, `side`, `leaves_qty` (quantity still open after this fill; 0 means the order is finished). `request_id` is 0: route by `order_id` |
| `CANCELLED` | the order left the book without filling the rest | `order_id`; `request_id` set for a user cancel, 0 for an IOC/FOK remainder (`status` FOK_FAILED or OK) |
| `MODIFIED` | a modify was applied | `request_id`, `order_id`, `qty`, `price` (0 = unchanged), `leaves_qty` |

Ordering guarantees, per session:

1. For a new order, `ACCEPTED` or `REJECTED` comes first, with your
   `request_id`.
2. Any fills caused by that order's arrival follow its `ACCEPTED`.
3. An IOC or FOK remainder is reported as `CANCELLED` after those fills.
4. A resting order's later fills arrive as `EXECUTION` whenever they
   happen. An execution that empties the order is the last report for it;
   no `CANCELLED` follows.
5. `match_id` is the same value the public feed shows in its Order Executed
   message, so you can tie private reports to the public stream.

`leaves_qty` is reported on every kind that changes it, so an adapter can
release its client id as soon as it sees an execution with `leaves_qty`
0, a `CANCELLED`, or a `REJECTED`.

## Reference data

```c
mktsim_symbol_info info[256];
size_t n = api->symbols(ex, info, 256);      // id + NUL-terminated ticker
uint32_t id; api->resolve_symbol(ex, "AAPL", 4, &id);
uint64_t t = api->now_ns();                  // wall clock, ns since epoch
api->log(ex, "my-adapter", "listening on :9000");
```

## Building an adapter

Compile against `api.h` only; no other exchange headers are needed.

```
clang++ -std=c++17 -O2 -fPIC -dynamiclib -Igateway/include my_adapter.cpp -o my_adapter.dylib   # macOS
g++     -std=c++17 -O2 -fPIC -shared     -Igateway/include my_adapter.cpp -o my_adapter.so      # Linux
```

Then list it in the exchange config:

```json
"ingress": [
  { "type": "boe", "port": 30000 },
  { "type": "plugin", "path": "path/to/my_adapter.dylib", "config": { "port": 9000 } }
]
```

Every entry runs; `"enabled": false` skips one. The worked example is
[`gateway/plugins/jsonl_ingress.cpp`](../gateway/plugins/jsonl_ingress.cpp),
a newline-delimited JSON protocol over TCP in about 300 lines, with a
Python client in `gateway/tools/jsonl_client.py`. The built-in BOE gateway
is the same kind of thing with a binary protocol; see
`gateway/src/boe_server.cpp`.

## What the exchange does behind the API

- Each shard has one lock-free multi-producer command queue; `submit`
  routes a new order by its symbol's shard and a cancel or modify by the
  shard encoded in the order id, so any number of sessions on any threads
  can submit without a lock.
- Each session has its own report queue filled by the shard engine
  threads. The engine thread of a shard keeps the order-to-session map
  for its orders, so routing a resting order's fill to its owner is a hash
  lookup on the thread that produced the fill.
- Validation that needs no engine state (symbol, quantity, side, type,
  time in force) happens in `submit` on the caller's thread and is
  reported as a return code, not a report.

## Versioning

`api->version` is `MKTSIM_INGRESS_API_VERSION`. A plugin must refuse a
version it does not know. Fields are only ever appended to the structs,
and new functions only ever appended to the table, within a major version.
