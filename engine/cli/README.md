# mktsim

Command-line front end for the matching engine. One command language, three
modes:

```
make cli                                   # builds bin/mktsim
make install                               # symlinks it to ~/.local/bin/mktsim
mktsim shell [config.json]                 # interactive, in-process exchange
mktsim run SCRIPT|- [config.json]      # script, in-process, exit 1 on failure
mktsim connect [host:port] [SCRIPT|-]  # same commands over BOE to exchange_server
mktsim tui [feedviz args]              # open the feed TUI (clients/feedviz)
mktsim up [config] [--flow ...] [-i|-y]   # start the exchange as a daemon (asks for the setup if no config)
mktsim init                            # interactive config builder, no start
mktsim down | status | logs [-f]       # stop it, inspect it, read its log
mktsim market list|up NAME|down|status # a Python scenario as the market
mktsim new strategy|market|flow|adapter NAME   # scaffold without questions
make cli-test                              # runs cli/scenarios/*.txt
```

`make install` puts a symlink in `~/.local/bin` (override with
`INSTALL_DIR=/usr/local/bin`); after that `mktsim` works from any directory.
The config defaults to `configs/default.json` relative to the working
directory, falling back to the repo the binary was built in, so an installed
`mktsim shell` finds it wherever you run it.

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
mktsim connect 127.0.0.1:30000 cli/scenarios/remote/remote.txt
```

**tui** hands off to the Rust feed viewer, `clients/feedviz`, which
subscribes to the multicast feed and draws every symbol's book live. Build it
once with `cd clients/feedviz && cargo build --release`. Any arguments are
passed through (`--replay FILE`, `--group`, `--port`, `--fps`, `--seconds`,
`--headless`). On macOS `--iface 127.0.0.1` is added unless you pass your own
`--iface` or `--replay`, matching the config's feed interface.

```
./gateway/build/exchange_server configs/default.json --rate 20000 &
mktsim tui
mktsim tui --replay ../clients/feedviz/testdata/capture.bin
```

## Running the exchange as a service

`mktsim up` with no config on a terminal walks through the setup in two
stages, like `npm init` or `create-next-app`. Arrow keys or j/k move in
lists, Enter confirms.

**1 · Engine**: symbols (all 100, a count, or your own list), shards, and the
market data multicast group.

**2 · Order flow**: where the orders come from.

- *a predefined flow*: pick a market to trade against. Built-in profiles
  (calm, normal, heavy) run inside the exchange. Python scenarios (random,
  hawkes, trending, volatile, load) are seeded agent populations from
  `python/mktsim/sim.py` that `mktsim up` starts as a market process next to
  the exchange; `mktsim market status|logs|down` manage it.
- *my own simulator*: you write the order flow.
  - *Python population* scaffolds `market.py`: a `Scenario` with a population
    built from `mktsim.agents` plus your own `Strategy` subclasses, a
    `Fundamental` value process, and scripted shocks. `--workers N` splits the
    agents over processes (about 5k orders/s with 4 workers).
  - *C++ flow model* scaffolds an in-process plugin: you write `Model::next()`
    in `src/model.hpp`, the generated adapter paces it over N sessions. Tens
    of thousands of orders a second (20k/s verified) with no sockets in the
    way. The wizard builds it with cmake and wires it into the config.
  - *existing library*: a shared library you already built against
    `gateway/include/ingress/api.h`. (`mktsim new adapter` scaffolds a C++
    protocol gateway for a custom wire format: TCP and sessions generated,
    you write `src/protocol.hpp`.)
- *both*: a predefined flow alongside your simulator.

**3 · Trading agents**: optionally scaffold `strategy.py`, a `Strategy`
subclass that trades against whatever market is running. It gets the public
view only (`self.market`: books from the feed, clock), never the simulation's
private state, so results mean what they would on a real venue.

Python projects get a copy of the client library (`python/mktsim` in this
repo): `Exchange` (buy, sell, cancel, modify, position, pnl), `Feed`
(multicast subscriber with snapshot bootstrap and L3 books), `Market`
(public view) and `sim.World` (private: `Fundamental`, shocks), `Strategy`,
`Hawkes`, `agents`, `Scenario` and `SCENARIOS`, `run` / `run_many`. Offline
tests: `make py-test`.

Without the questions: `mktsim new strategy|market|flow|adapter NAME [DIR]`
scaffolds a project, `mktsim market up hawkes --workers 4` runs a scenario.

Then where to save the config, a summary, next steps, and whether to start.

`mktsim init` asks the same questions and only writes the config.
`mktsim up -y` skips the questions and uses `configs/default.json`;
`mktsim up path.json` uses a config you already have; `-i` forces the questions.

```
mktsim up                     # interactive setup, then start
mktsim up --flow busy         # same, with the built-in flowgen set to the busy profile
mktsim up --flow off          # no synthetic flow: bring your own ingress
mktsim status                 # pid, uptime, symbols, shards, ports, last log line
mktsim logs -f                # follow the log
mktsim tui                    # attach the TUI whenever
mktsim down                   # graceful stop (SIGTERM, waits up to 10 s)
```

The exchange keeps running until `mktsim down`, independent of any terminal.
State lives in `${XDG_STATE_HOME:-~/.local/state}/mktsim/`: `exchange.pid`,
`exchange.log`, and with `--flow` the derived `exchange.json` that was
actually passed to the server. `exchange_server --daemon|--stop|--status`
use the same files, so the two tools agree.

## Scenarios

`cli/scenarios/` holds runnable examples. `basic.txt`, `tif.txt` and
`flow.txt` run in-process via `make cli-test`; `remote/remote.txt` needs a live
`exchange_server`.
