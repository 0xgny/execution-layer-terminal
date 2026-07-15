# Execution Layer

A real-time paper-trading terminal spanning two asset classes. Crypto streams
live from Coinbase into a KDB+ tick store; stocks come from Alpaca's free
market-data API (real IEX-venue real-time quotes + historical daily bars). A
C++ "Bloomberg-style" terminal reads both over IPC/REST to let you trade a
simulated account with real-time PnL, live price and PnL charts, and an order
blotter.

It fuses two earlier projects:

- q-sim: a KDB+/PyKX real-time tick pipeline (the streaming half)
- Stock-Analysis-Engine: statistical / ML market analysis (the research half)

Everything is paper trading. No real orders are ever routed.

---

## Architecture

```
  Coinbase WebSocket                          Alpaca REST (IEX real-time)
         |                                            |
         v                                            v
  Feedhandler (Python)                          AlpacaClient (libcurl)
         |                                            |
         v                                            v
  Tickerplant  tp.q  :5010               StockFeed (own background thread,
         |                               polls + caches, rate-limit aware)
         v                                            |
  Real-time DB  rdb.q  :5011                          |
         |  (q IPC over a plain socket)                |
         v                                            v
                    C++ terminal
    KdbClient + StockFeed  ->  TradingEngine (background thread)  ->  ImGui/ImPlot GUI
                                OMS + risk gate + paper matching + portfolio/PnL
```

The KDB+ tickerplant/RDB is the shared low-latency backbone for crypto. Python
does ingestion; C++ does execution and the UI; both speak to KDB+ over IPC.
Stocks are a second, independent source: the C++ terminal talks to Alpaca
directly over HTTPS (no Python/KDB+ involved), on its own thread so a slow HTTP
round-trip never stalls crypto polling or the GUI. Swapping either data source
is isolated to one class (`BaseFeedHandler` subclass for crypto venues,
`AlpacaClient`/`StockFeed` for stocks).

For a complete, detailed walkthrough of the architecture, frameworks, the q IPC
protocol, the threading model, and traced end-to-end workflows, see `design.md`.
`docs/architecture.md` covers the higher-level design and roadmap.

---

## Prerequisites

- macOS (Apple Silicon supported) or Linux
- KDB+ / q with a license (the free `kc.lic` community license is fine) -- crypto only
- Python 3.11 (PyKX does not yet support 3.14 cleanly - see Gotchas) -- crypto only
- A C++20 compiler (Apple clang works)
- For the GUI: GLFW, plus the vendored Dear ImGui, ImPlot, and nlohmann/json (fetched below)
- libcurl (ships with macOS/most Linux distros) -- for the stock feed
- Optional, for stocks: a free [Alpaca](https://alpaca.markets) account and API
  key/secret. Without it the terminal runs exactly as before, crypto-only.

---

## How to run

The system runs as two processes: the data stack (one command) and the terminal.

### 1. One-time setup

```bash
cd execution-layer

# Python environment for the feedhandler
python3.11 -m venv .venv
. .venv/bin/activate
pip install -r feedhandler/requirements.txt

# GUI dependencies
brew install glfw
cd cpp/third_party
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git
git clone --depth 1 https://github.com/epezent/implot.git
mkdir -p json && curl -fsSL -o json/json.hpp \
  https://github.com/nlohmann/json/releases/latest/download/json.hpp
cd ../..

# Tell q where its home + license live (adjust to your install)
export QHOME="$HOME/.kx/q"
export QLIC="$HOME/.kx"

# Optional: enables the Stocks tab (Alpaca free market-data plan). Without
# these the terminal runs crypto-only, same as before.
export ALPACA_API_KEY_ID="..."
export ALPACA_API_SECRET_KEY="..."
```

### 2. Start the data stack (terminal 1)

```bash
cd execution-layer
export QHOME="$HOME/.kx/q" QLIC="$HOME/.kx"
scripts/run_stack.sh coinbase
```

This launches the tickerplant, the RDB, and the Coinbase feedhandler. With no
symbol argument it boots the top-crypto universe (~95 live USD products), so the
terminal is populated out of the box. To stream a specific set instead:

```bash
scripts/run_stack.sh coinbase BTC-USD,ETH-USD
```

Wait until you see `[coinbase] connected + subscribed to N products`.

### 3. Build and run the terminal (terminal 2)

```bash
cd execution-layer/cpp
make gui
./build/terminal
```

The terminal is a plain socket client and does NOT need QHOME/QLIC. Optional
arguments: `./build/terminal <rdb-host> <rdb-port>` (default `127.0.0.1 5011`).

### 4. Trade

1. Enter your initial capital and click START DESK.
2. Market Watch fills with the live crypto universe (Crypto tab). If Alpaca is
   configured, switch to the Stocks tab and type an exact ticker (e.g. `AAPL`)
   in "+ Add Stock" -- there's no bulk catalog to browse for stocks, unlike
   crypto's Ticker Search, so you type the symbol directly.
3. Click any row to chart it.
4. In the Order Ticket, pick a symbol, choose `$` notional or exact `Qty`, and
   BUY / SELL.
5. Watch the price chart and the PnL chart (green in profit, red in loss).
6. Close a position with SELL ALL (in the ticket or per-row "flatten" in
   Positions > Current). It then moves to Positions > Previous with its
   entry/exit/realized PnL. Click any Current position to jump to the Order
   Ticket pre-filled at its exact size, to top up or exit precisely.

To stop: Ctrl-C in terminal 1 (it tears down the q processes).

---

## Running without a GUI / display

The trading logic can be exercised headlessly against the live RDB:

```bash
cd execution-layer/cpp
make run-selftest    # single-threaded: fund, buy live, watch PnL, flatten
make engine-test     # exercises the threaded engine + command/view handoff
```

---

## Project layout

```
execution-layer/
  kdb/                     KDB+ tick infrastructure (q)
    schema.q               trade / quote table definitions (the shared contract)
    tp.q                   tickerplant: pub/sub router + control plane (:5010)
    rdb.q                  real-time DB: tick store + last-value caches (:5011)
  feedhandler/             Python market-data feedhandler
    schema.py              normalized Trade / Quote types
    base.py                buffering, flush loop, TLS, control loop (abstract)
    coinbase.py            Coinbase WebSocket + catalog + dynamic subscribe
    binance.py             Binance WebSocket implementation
    mock.py                synthetic feed (no network) for offline testing
    publisher.py           PyKX bridge: batches -> .u.upd on the tickerplant
    universe.py            default top-crypto universe subscribed at boot
    __main__.py            CLI entrypoint (--venue coinbase|binance|mock)
  cpp/                     C++ execution terminal
    include/execution/     types, portfolio, risk, oms, matching, kdb_client,
                            alpaca_client, stock_feed, engine
    src/                   implementations + terminal_gui.cpp + headless tests
    Makefile               build (make gui / make all / make alpaca-test)
    README.md              terminal design + build detail
  scripts/run_stack.sh     launch tp + rdb + feedhandler together
  docs/                    design docs and decision records
  web-version/             a separate, SELF-CONTAINED project (hosted,
                            multi-user React + C++ backend version -- see
                            web-version/README.md and web-version/design.md).
                            It started as a companion sharing this repo's
                            kdb/, feedhandler/, and cpp/'s trading core, but
                            now copies the trading core in and connects to
                            Coinbase directly (no KDB+/feedhandler at all),
                            specifically so it can move to its own repo.
```

---

## What is verified

Tested end to end on Apple Silicon:

- Coinbase live -> tickerplant -> RDB -> C++ client: real quotes for ~95 symbols.
- Buying/selling on live prices with correct cash, position, and PnL accounting;
  self-test showed PnL tracking the market in real time, then flattening cleanly.
- Control plane: requesting a new ticker at runtime dynamically subscribes the
  feed and it starts streaming, no restart.
- snap[] latency ~0.085 ms for 95 symbols (via the RDB last-value caches).
- Parsing unit tests for Binance and Coinbase wire formats.

---

## Known gotchas

- Use Python 3.11, not 3.14. PyKX 4.0 on 3.14 raises numpy deallocator errors and
  can crash if you publish from a worker thread. 3.11 is clean.
- PyKX embedded q is not thread-safe: the feedhandler only publishes from its main
  thread on purpose.
- q needs QHOME and QLIC. If you see "license error: no license loaded", point
  QLIC at the directory containing your kc.lic.
- TLS behind an intercepting proxy: if the feed gets CERTIFICATE_VERIFY_FAILED,
  the feedhandler uses certifi by default; for local debugging only you can set
  EL_INSECURE_SSL=1 (never for order routing).
- Binance is geo-blocked in some regions (HTTP 451). Use Coinbase there.

---

## Roadmap

Done: live feedhandler (Binance + Coinbase); tickerplant/RDB with caches; native
C++ KDB+ client; threaded engine with paper OMS, risk gate, and matching; docked
Bloomberg-style GUI with account bar, tabbed market watch (crypto + stocks),
ticker search, order ticket, tabbed positions (current/previous), event log, and
live price + PnL charts; runtime control plane to add any crypto ticker;
top-crypto boot universe; stocks via Alpaca's free real-time IEX data API on a
dedicated polling thread.

Next: candlestick / OHLC charts with historical backfill; an Analysis Engine that
publishes signals into a KDB+ `signal` table for the engine to consume; end-of-day
persistence to an on-disk HDB; optional live order routing to an exchange testnet
behind the risk gate.
```
