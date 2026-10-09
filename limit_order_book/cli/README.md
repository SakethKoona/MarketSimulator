# mktsim

Command-line front end for the matching engine. One command language, three
modes:

```
make cli                                   # builds bin/mktsim
bin/mktsim shell [config.json]             # interactive, in-process exchange
bin/mktsim run SCRIPT|- [config.json]      # script, in-process, exit 1 on failure
bin/mktsim connect [host:port] [SCRIPT|-]  # same commands over BOE to exchange_server
bin/mktsim tui [feedviz args]              # open the feed TUI (clients/feedviz)
make cli-test                              # runs cli/scenarios/*.txt
```

The config defaults to `configs/default.json` found relative to the working
directory (also `../configs`, `../../configs`).

## Commands

```
buy  SYM QTY @ PRICE [ioc|fok]   limit order, GTC unless ioc/fok
sell SYM QTY @ PRICE [ioc|fok]
buy  SYM QTY market              market order
cancel ID
modify ID QTY                    reduce quantity, keeps priority
modify ID QTY @ PRICE            cancel/replace: same id, new priority
book SYM                         L3: every order at every level   (in-process)
l2 SYM [DEPTH]                   aggregated levels                (in-process)
top                              best bid/ask per symbol          (in-process)
order ID                         one order's state and queue position (in-process)
events [N]                       last N engine events, default all pending (in-process)
flow SYM N [SEED]                run N synthetic operations on SYM (in-process)
symbols                          id, ticker and shard             (in-process)
echo on|off                      print engine events after every command
expect ok|reject CMD ...         assert the outcome of an order command
help, quit
```

Order ids are the exchange's ids, the same values that appear on the public
feed. They can be written as the raw number, as `shard:seq`, as `$last`
(the most recently acknowledged order), or as `$N` (the Nth acknowledged
order in this session). With more than one shard, acks print both forms.

Lines starting with `#` are comments. In `run` and scripted `connect` mode,
a parse error or a failed `expect` makes the exit status 1; a plain rejection
is printed and does not.

## Modes

**shell / run** embed an `Exchange` in the process: no network, full book
inspection, and `flow` for synthetic activity. This is the sandbox for
understanding what the matching engine does.

**connect** speaks BOE (docs/protocol/boe-v1.md) to a running
`exchange_server`. Orders, cancels and modifies go over TCP and the acks,
executions and rejects come back on the connection. Fills for your resting
orders that happen while you are at the prompt are printed before the next
prompt, as are cancels initiated by the exchange. There is no snapshot message
in BOE, so `book`, `l2`, `top`, `order`, `events`, `flow` and `symbols` say so
and do nothing. Cancel and modify only work on orders this session placed.

```
./gateway/build/exchange_server configs/default.json --rate 5 &
bin/mktsim connect 127.0.0.1:30000 cli/scenarios/remote/remote.txt
```

**tui** hands off to the Rust feed viewer, `clients/feedviz`, which
subscribes to the multicast feed and draws every symbol's book live. Build it
once with `cd clients/feedviz && cargo build --release`. Any arguments are
passed through (`--replay FILE`, `--group`, `--port`, `--fps`, `--seconds`,
`--headless`). On macOS `--iface 127.0.0.1` is added unless you pass your own
`--iface` or `--replay`, matching the config's feed interface.

```
./gateway/build/exchange_server configs/default.json --rate 20000 &
bin/mktsim tui
bin/mktsim tui --replay ../clients/feedviz/testdata/capture.bin
```

## Scenarios

`cli/scenarios/` holds runnable examples. `basic.txt`, `tif.txt` and
`flow.txt` run in-process via `make cli-test`; `remote/remote.txt` needs a live
`exchange_server`.
