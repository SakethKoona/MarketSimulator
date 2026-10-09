#!/usr/bin/env bash
# Starts the exchange, order flow and the TUI. With tmux: three panes in one
# window; without: exchange and flowgen in the background, feedviz in front.
#
#   scripts/demo.sh [--profile calm|busy] [--rate N] [--sessions N] [--theme NAME]
set -euo pipefail
cd "$(dirname "$0")/.."

PROFILE=calm RATE="" SESSIONS=4 THEME=amber IFACE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --profile) PROFILE=$2; shift 2;;
    --rate) RATE=$2; shift 2;;
    --sessions) SESSIONS=$2; shift 2;;
    --theme) THEME=$2; shift 2;;
    *) echo "usage: $0 [--profile calm|busy] [--rate N] [--sessions N] [--theme NAME]"; exit 2;;
  esac
done

EX=build/gateway/exchange_server
FG=build/gateway/flowgen
FV=clients/feedviz/target/release/feedviz
for b in "$EX" "$FG" "$FV"; do
  [ -x "$b" ] || { echo "missing $b; run: make"; exit 1; }
done
# Same-host multicast on macOS needs the loopback interface on both sides.
case "$(uname -s)" in Darwin) IFACE="--iface 127.0.0.1";; esac
FLOW_ARGS="--sessions $SESSIONS --profile $PROFILE"
[ -n "$RATE" ] && FLOW_ARGS="$FLOW_ARGS --rate $RATE"

if command -v tmux >/dev/null 2>&1 && [ -z "${NO_TMUX:-}" ]; then
  S=mktsim
  tmux kill-session -t $S 2>/dev/null || true
  tmux new-session -d -s $S -n demo "$EX configs/default.json"
  tmux split-window -t $S -v -l 8 "sleep 0.7; $FG $FLOW_ARGS"
  tmux split-window -t $S:0.0 -h -l 70% "sleep 1.0; $FV $IFACE --theme $THEME"
  tmux select-pane -t $S:0.2
  exec tmux attach -t $S
else
  "$EX" configs/default.json > /tmp/mktsim-exchange.log 2>&1 &
  EXP=$!
  sleep 0.7
  "$FG" $FLOW_ARGS > /tmp/mktsim-flowgen.log 2>&1 &
  FGP=$!
  trap 'kill $FGP $EXP 2>/dev/null' EXIT
  sleep 0.5
  "$FV" $IFACE --theme "$THEME"
fi
