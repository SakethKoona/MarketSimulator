#!/usr/bin/env bash
# The user flow: an exchange that runs as a long-lived service with the
# built-in demo flow (or your own ingress adapter), and a TUI you attach
# whenever you like.
#
#   scripts/demo.sh up [config]      start the exchange in the background
#   scripts/demo.sh tui [args]       attach the TUI (any time, as often as you like)
#   scripts/demo.sh status | logs | down
#   scripts/demo.sh                  up + tui
set -euo pipefail
cd "$(dirname "$0")/.."
EX=build/gateway/exchange_server
FV=clients/feedviz/target/release/feedviz
CFG=${CFG:-configs/default.json}
case "$(uname -s)" in Darwin) IFACE="--iface 127.0.0.1";; *) IFACE="";; esac
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/mktsim

cmd=${1:-}; shift || true
case "$cmd" in
  up)     [ -x "$EX" ] || { echo "missing $EX; run: make"; exit 1; }
          "$EX" "${1:-$CFG}" --daemon ;;
  down)   "$EX" --stop ;;
  status) "$EX" --status ;;
  logs)   tail -n 50 -f "$STATE/exchange.log" ;;
  tui)    [ -x "$FV" ] || { echo "missing $FV; run: make"; exit 1; }
          exec "$FV" $IFACE "$@" ;;
  "")     [ -x "$EX" ] && [ -x "$FV" ] || { echo "run: make"; exit 1; }
          "$EX" --status >/dev/null 2>&1 || "$EX" "$CFG" --daemon
          sleep 0.5
          exec "$FV" $IFACE "$@" ;;
  *)      echo "usage: $0 [up [config] | tui [args] | status | logs | down]"; exit 2 ;;
esac
