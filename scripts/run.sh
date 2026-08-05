#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run.sh -- build and run the terminal locally.
#
# This is the only script you need. It builds the GUI if it's out of date,
# starts the market-data feedhandler in the background, launches the terminal,
# and shuts the feedhandler down when you close the window.
#
# Usage:
#   scripts/run.sh                      # live Coinbase data (default)
#   scripts/run.sh mock                 # synthetic prices, no network
#   scripts/run.sh binance BTCUSDT,ETHUSDT
#   scripts/run.sh coinbase BTC-USD,ETH-USD
#   scripts/run.sh --feed-only          # just the feed (for engine_test)
#   scripts/run.sh --no-feed            # just the terminal
#
# With no symbol list, Coinbase boots the full top-crypto universe (~95 products).
# ---------------------------------------------------------------------------
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

FEED_PORT="${EL_FEED_PORT:-5020}"
WANT_FEED=1
WANT_GUI=1

case "${1:-}" in
  --feed-only) WANT_GUI=0; shift ;;
  --no-feed)   WANT_FEED=0; shift ;;
esac

VENUE="${1:-coinbase}"
if [[ "$VENUE" == "coinbase" ]]; then
  SYMBOLS="${2:-}"            # empty => full top-crypto universe
else
  SYMBOLS="${2:-BTCUSDT,ETHUSDT}"
fi

# --- python ----------------------------------------------------------------
if [[ -x "$ROOT/.venv/bin/python" ]]; then
  PY="$ROOT/.venv/bin/python"
else
  PY="$(command -v python3 || true)"
  [[ -n "$PY" ]] || { echo "[run] no python3 found; see README setup" >&2; exit 1; }
  echo "[run] no .venv found, using $PY (see README if imports fail)"
fi

# --- build -----------------------------------------------------------------
if ((WANT_GUI)); then
  echo "[run] building terminal"
  make -C cpp gui
fi

# --- feed ------------------------------------------------------------------
FEED_PID=""
cleanup() {
  if [[ -n "$FEED_PID" ]] && kill -0 "$FEED_PID" 2>/dev/null; then
    echo "[run] stopping feedhandler"
    kill "$FEED_PID" 2>/dev/null || true
    wait "$FEED_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

if ((WANT_FEED)); then
  echo "[run] starting feedhandler (venue=$VENUE symbols=${SYMBOLS:-<full universe>})"
  if [[ -n "$SYMBOLS" ]]; then
    "$PY" -u -m feedhandler --venue "$VENUE" --symbols "$SYMBOLS" --feed-port "$FEED_PORT" &
  else
    "$PY" -u -m feedhandler --venue "$VENUE" --feed-port "$FEED_PORT" &
  fi
  FEED_PID=$!
fi

# --- terminal --------------------------------------------------------------
if ((WANT_GUI)); then
  echo "[run] launching terminal -- click START DESK to begin"
  echo "[run] (the feed reports 'dropping ticks' until you do; that's expected)"
  ./cpp/build/terminal "$FEED_PORT"
else
  echo "[run] feed-only; Ctrl-C to stop"
  wait "$FEED_PID"
fi
