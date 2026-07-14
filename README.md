# Execution Layer

A real-time crypto trading platform that fuses two prior projects:

- **[q-sim](https://github.com/0xgny/q-sim)** — a KDB+/PyKX real-time tick pipeline (the *streaming* half)
- **[Stock-Analysis-Engine](https://github.com/0xgny/Stock-Analysis-Engine)** — statistical + ML market analysis (the *research* half)

The goal is an end-to-end loop: **stream live market data → analyze it on the fly → act on it** through an execution layer where users express strategies and trade signals. The execution engine is planned in **C++** with a Bloomberg-terminal-style GUI.

> **Status:** Live end-to-end. Coinbase/Binance market data → KDB+ → a native C++
> client → a threaded paper-trading engine → a **Bloomberg-style docked GUI**
> (Dear ImGui) with real-time PnL, order tickets, positions, and a kill switch.
> Trading logic verified against live Coinbase quotes. See [Roadmap](#roadmap).

---

## The big picture

```
┌─────────────┐  WebSocket   ┌──────────────┐  IPC   ┌────────────┐
│ Crypto exch │ ───────────▶ │ Feedhandler  │ ─────▶ │Tickerplant │  (pub/sub router)
│ (Binance…)  │ trades/quotes│ (Python)     │ .u.upd │  tp.q      │
└─────────────┘              └──────────────┘        └─────┬──────┘
                                                           │ publishes
                                                  ┌────────┴────────┐
                                                  ▼                 ▼
                                             ┌─────────┐      ┌──────────┐
                                             │  RDB    │      │  HDB     │ (planned)
                                             │ in-mem  │      │ on-disk  │
                                             │ rdb.q   │      │ history  │
                                             └────┬────┘      └──────────┘
                                                  │ PyKX / C API
                              ┌───────────────────┴───────────────────┐
                              ▼                                        ▼
                     ┌─────────────────┐                  ┌────────────────────────┐
                     │ Analysis Engine │  signals         │ Execution Layer (C++)  │
                     │ (Python/ML)     │ ───────────────▶ │ OMS + risk + matching  │
                     └─────────────────┘                  │ ImGui Bloomberg GUI    │
                                                          └────────────────────────┘
```

The KDB+ tickerplant/RDB is the shared low-latency backbone. Python does research;
C++ does execution; both speak to KDB+ over its native IPC. Swapping the data
**source** (exchange) is isolated to one feedhandler class — everything downstream
is unaffected. See [`docs/architecture.md`](docs/architecture.md).

---

## What's here now

```
execution-layer/
├── kdb/                       # KDB+ tick infrastructure (q)
│   ├── schema.q               # trade / quote table definitions (the shared contract)
│   ├── tp.q                   # tickerplant: pub/sub router (port 5010)
│   └── rdb.q                  # real-time DB: in-memory store + queries (port 5011)
├── feedhandler/               # Python market-data feedhandler
│   ├── schema.py              # normalized Trade / Quote dataclasses
│   ├── config.py              # env-driven configuration
│   ├── publisher.py           # PyKX bridge: batches -> .u.upd on the tickerplant
│   ├── base.py                # buffering + flush loop + TLS + lifecycle (abstract)
│   ├── binance.py             # Binance WebSocket implementation
│   ├── coinbase.py            # Coinbase WebSocket implementation
│   ├── mock.py                # synthetic feed (no network) for offline testing
│   ├── __main__.py            # CLI entrypoint (--venue binance|coinbase|mock)
│   └── test_*_parsing.py      # deterministic wire-format tests (binance, coinbase)
├── cpp/                       # C++ execution core (paper OMS + risk + matching)
│   ├── include/execution/     # types, risk, oms, matching, strategy, market_data
│   ├── src/                   # implementations + main.cpp (Bloomberg-style blotter)
│   ├── Makefile / CMakeLists.txt
│   └── README.md              # execution-layer design + roadmap
├── scripts/run_stack.sh       # launch tp + rdb + feedhandler together
└── docs/                      # design docs & decision records
```

---

## Quickstart

**Prerequisites:** KDB+ (`q`) with a license (community `kc.lic` is fine), and Python 3.11.

```bash
cd execution-layer

# 1. Python environment (3.11 — see the note below on 3.14)
python3.11 -m venv .venv
. .venv/bin/activate
pip install -r feedhandler/requirements.txt

# 2. Make sure q can find its home + license
export QHOME="$HOME/.kx/q" QLIC="$HOME/.kx"   # adjust to your install

# 3a. One-shot: bring up the whole stack with the offline mock feed
scripts/run_stack.sh mock BTCUSDT,ETHUSDT

# 3b. …or with the live Coinbase feed (works from most locations)
scripts/run_stack.sh coinbase BTC-USD,ETH-USD

# 3c. …or the live Binance feed (where not geo-blocked — see below)
scripts/run_stack.sh binance BTCUSDT,ETHUSDT
```

Then, from another shell, query the live in-memory data:

```bash
q   # or: $HOME/.kx/bin/q
q) h:hopen `::5011
q) h "counts[]"                                             / row counts + latency
q) h "select vwap:size wavg price, vol:sum size by sym from trade"
```

### Running the pieces by hand

```bash
q kdb/tp.q     # terminal 1 — tickerplant on 5010
q kdb/rdb.q    # terminal 2 — RDB on 5011, subscribes to the tickerplant
python -m feedhandler --venue mock --symbols BTCUSDT,ETHUSDT   # terminal 3
```

---

## Verified

Tested end-to-end on this machine:

- **Coinbase live → tickerplant → RDB → query**: real market data flows — e.g.
  BTC-USD ≈ \$62k, ETH-USD ≈ \$1.77k, ~12 ms ingestion latency, with live
  VWAP and buy/sell order-flow imbalance computed by pushed-down q queries.
- **Mock feed → tickerplant → RDB → query**: 200 trades + 200 quotes, correct
  nanosecond timestamps, pushed-down VWAP (offline, no network).
- **Parsing tests** (`test_binance_parsing.py`, `test_coinbase_parsing.py`) pass:
  aggressor-side logic (incl. Coinbase's maker→aggressor inversion), price/size,
  timestamp conversion, and top-of-book → `quote` mapping.
- **C++ execution core** (`cpp/`) builds clean (C++20, no warnings) and runs the
  full paper pipeline: MA-cross signals → risk gate → paper fills → position/PnL,
  with a Bloomberg-style blotter; the risk demo shows an oversized order rejected
  and the kill switch halting trading.
- **Binance live**: connects/reconnects correctly, but this location is
  **geo-blocked by Binance.com** (HTTP 451) — the jurisdiction caveat in
  [`docs/decisions/0001-exchange-choice.md`](docs/decisions/0001-exchange-choice.md).
  Use Coinbase here.

---

## Known environment gotchas

- **Use Python 3.11, not 3.14.** PyKX 4.0 + numpy on Python 3.14 raises
  `decref_numpy_allocated_data` deallocator errors (cosmetic) and, if you ever
  publish from a worker thread, can SIGSEGV. Python 3.11 is clean.
- **PyKX embedded q is not thread-safe.** Always publish from the main/event-loop
  thread. The feedhandler is written this way on purpose.
- **q needs `QHOME` + `QLIC`.** If you see `license error: no license loaded`,
  point `QLIC` at the directory containing your `kc.lic`.
- **TLS behind a corporate/intercepting proxy:** if you get
  `CERTIFICATE_VERIFY_FAILED`, the feedhandler uses `certifi` by default; for
  local debugging only you can set `EL_INSECURE_SSL=1` (never for order routing).
- **Binance HTTP 451:** you're in a restricted location. Use Coinbase (planned)
  or Binance.US, or run from an eligible location.

---

## Roadmap

1. **✅ Feedhandler → KDB+**: live crypto ticks (Binance + Coinbase) into the tickerplant/RDB.
2. **✅ C++ execution core**: OMS state machine, paper matching engine, and risk
   controls (limits + kill switch). *(paper only)*
3. **✅ Native KDB+ client + threaded engine**: C++ pulls live quotes from the RDB
   over a pure-socket q IPC client; `TradingEngine` runs the desk on a background thread.
4. **✅ Bloomberg-style docked GUI** (Dear ImGui + ImPlot): fund capital on launch,
   account bar, market watch (+ add any ticker), order ticket, positions, kill switch.
5. **Control plane**: auto-subscribe the feedhandler when the GUI watches a new
   ticker (today the feed must already carry that symbol — see `cpp/README.md`).
6. **ImPlot charts** per symbol + per-ticker detail windows.
7. **Analysis Engine → `signal` table**: Stock-Analysis-Engine metrics on rolling
   windows from the RDB, published into KDB+; the engine consumes them.
8. **HDB + end-of-day flush**: persist intraday ticks to a partitioned on-disk HDB.
9. **Stocks** via a polled Yahoo feed (`exch=yahoo`) into the same pipeline.
10. **(Optional) live order routing** to exchange testnets, behind the risk gate.

See [`docs/architecture.md`](docs/architecture.md) for the full design and
[`docs/phase-1-feedhandler.md`](docs/phase-1-feedhandler.md) for this phase in detail.
```
