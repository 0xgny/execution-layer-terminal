#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run_stack.sh -- run the market-data feed for local development.
#
# The feedhandler publishes ticks straight into the C++ terminal over a
# localhost socket (default 127.0.0.1:5020). There is nothing else to start:
# the terminal stores its own quotes and history in-process.
#
# Order doesn't matter. If the terminal isn't up yet (or you haven't clicked
# START DESK), the feedhandler drops ticks and retries until it is.
#
# Usage:
#   scripts/run_stack.sh              # defaults to the mock venue (no network)
#   scripts/run_stack.sh coinbase     # live, full top-crypto universe
#   scripts/run_stack.sh binance BTCUSDT,ETHUSDT
# ---------------------------------------------------------------------------
set -euo pipefail

VENUE="${1:-mock}"
# Default symbols are venue-native: Coinbase uses dash pairs (BTC-USD),
# Binance/mock use concatenated pairs (BTCUSDT). For Coinbase with no explicit
# symbols we leave it empty so the feedhandler boots the full top-crypto universe.
if [[ "$VENUE" == "coinbase" ]]; then
  SYMBOLS="${2:-}"
else
  SYMBOLS="${2:-BTCUSDT,ETHUSDT}"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Prefer the project venv's python if present.
if [[ -x "$ROOT/.venv/bin/python" ]]; then
  PY="$ROOT/.venv/bin/python"
else
  PY="python3"
fi

echo "[run_stack] root=$ROOT venue=$VENUE symbols=${SYMBOLS:-<full universe>}"
echo "[run_stack] starting feedhandler (Ctrl-C to stop)"

if [[ -n "$SYMBOLS" ]]; then
  exec "$PY" -u -m feedhandler --venue "$VENUE" --symbols "$SYMBOLS"
else
  exec "$PY" -u -m feedhandler --venue "$VENUE"
fi
