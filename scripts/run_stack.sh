#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# run_stack.sh — bring up the Phase 1 tick pipeline for local development.
#
# Launches, in order:
#   1. tickerplant (kdb/tp.q)  on port 5010
#   2. RDB         (kdb/rdb.q) on port 5011, subscribes to the tickerplant
#   3. feedhandler (python -m feedhandler)   publishes ticks into the tickerplant
#
# The q processes are started in the background with logs under logs/.
# The feedhandler runs in the foreground; Ctrl-C stops it, then we tear the
# q processes down.
#
# Usage:
#   scripts/run_stack.sh              # defaults to the mock venue (no network)
#   scripts/run_stack.sh binance BTCUSDT,ETHUSDT
# ---------------------------------------------------------------------------
set -euo pipefail

VENUE="${1:-mock}"
# Default symbols are venue-native: Coinbase uses dash pairs (BTC-USD),
# Binance/mock use concatenated pairs (BTCUSDT).
if [[ "$VENUE" == "coinbase" ]]; then
  SYMBOLS="${2:-BTC-USD,ETH-USD}"
else
  SYMBOLS="${2:-BTCUSDT,ETHUSDT}"
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p logs

# Resolve the q binary (KDB+ typically installs under ~/.kx or via QHOME).
Q_BIN="$(command -v q || echo "$HOME/.kx/bin/q")"

# q needs QHOME (install dir) and QLIC (dir containing the .lic). Default to the
# standard KX install layout if the caller hasn't set them.
export QHOME="${QHOME:-$HOME/.kx/q}"
export QLIC="${QLIC:-$HOME/.kx}"

# Prefer the project venv's python if present.
if [[ -x "$ROOT/.venv/bin/python" ]]; then
  PY="$ROOT/.venv/bin/python"
else
  PY="python"
fi

echo "[run_stack] root=$ROOT venue=$VENUE symbols=$SYMBOLS"

cleanup() {
  echo "[run_stack] stopping q processes..."
  [[ -n "${TP_PID:-}" ]] && kill "$TP_PID" 2>/dev/null || true
  [[ -n "${RDB_PID:-}" ]] && kill "$RDB_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "[run_stack] starting tickerplant -> logs/tp.log"
"$Q_BIN" kdb/tp.q </dev/null >logs/tp.log 2>&1 &
TP_PID=$!
sleep 1

echo "[run_stack] starting RDB -> logs/rdb.log"
"$Q_BIN" kdb/rdb.q </dev/null >logs/rdb.log 2>&1 &
RDB_PID=$!
sleep 1

echo "[run_stack] starting feedhandler (Ctrl-C to stop)"
"$PY" -u -m feedhandler --venue "$VENUE" --symbols "$SYMBOLS"
