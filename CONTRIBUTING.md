# Contributing

## Layout and ownership

```
engine/        matching engine, order books, shards, event sink, mktsim CLI, bench
gateway/       feed publisher (ITCH-style over MoldUDP64), ingress core + adapters, tools
clients/       feedviz (Rust TUI); client SDKs
docs/          wire specs (the contracts) and the ingress API
configs/       exchange configuration
```

Two rules that keep the pieces independent:

1. **The spec is the contract.** `docs/protocol/feed-v1.md`, `docs/protocol/boe-v1.md`
   and `docs/ingress-api.md` are what clients and adapters are written
   against. A wire change is a spec change first, then the C++ structs
   (`gateway/include/protocol`), then the Rust structs (`clients/feedviz/crates/protocol`),
   each with size assertions, and the reconstruction test must still pass.
2. **Engine and gateway are separate worlds.** The gateway talks to the engine
   only through `Exchange`'s public API and `EventSink`. Engine changes that
   alter what is emitted must keep `gateway/tests/feed_reconstruct_test` green.

## Build and test

```
make            # CMake build of engine + gateway + plugins, then cargo build
make test       # engine suite, gateway suite, Rust tests
make demo       # exchange + flowgen + feedviz in tmux
```

`cmake --build build` and `cargo test` in `clients/feedviz` work on their own.
CI runs the same on macOS and Linux; keep it green.

## Style

- C++20, `clang-format` with the repo config; no exceptions across the
  ingress C ABI; nothing allocates on the engine thread's hot path.
- Rust 2021, `rustfmt` with the repo config; the UI thread never iterates
  messages, only snapshots.
- Commit messages: what changed and why, measured numbers when a change
  claims performance.

## Adding an ingress adapter

Build against `gateway/include/ingress/api.h` only, list it in the config's
`ingress` array, and read `docs/ingress-api.md`. `gateway/plugins/jsonl_ingress.cpp`
is the worked example.
