# Execution Layer

A real-time paper-trading terminal spanning two asset classes. Crypto streams
live from Coinbase; stocks come from Alpaca's free market-data API (real
IEX-venue real-time quotes + historical daily bars). A C++ "Bloomberg-style"
terminal consumes both to let you trade a simulated account with real-time PnL,
live price and PnL charts, and an order blotter.

It grew out of q-sim, an earlier KDB+/PyKX tick pipeline; the streaming design
carried over, the KDB+ dependency did not (see `designReview.md`). A research /
signal layer is planned but **not built** -- see `architecture.md` Sec. 4.

Everything is paper trading. No real orders are ever routed.

---

## Architecture

```
  Coinbase WebSocket                          Alpaca REST (IEX real-time)
         |                                            |
         v                                            v
  Feedhandler (Python)                          AlpacaClient (libcurl)
         |  NDJSON batches over                       |
         |  127.0.0.1:5020                            v
         |                               StockFeed (own background thread,
         |                               polls + caches, rate-limit aware)
         v                                            |
  ============================ C++ terminal ==========================
    FeedServer -> MarketStore  +  StockFeed
                       |                              |
                       v                              v
              TradingEngine (background thread)  ->  ImGui/ImPlot GUI
              OMS + risk gate + paper matching + portfolio/PnL
```

Everything the terminal needs lives inside the terminal. `MarketStore` is an
in-process last-value cache plus a bounded per-symbol price history; the Python
feedhandler publishes batched ticks into it over a localhost socket. Python does
ingestion, C++ does storage, execution, and the UI.

Stocks are a second, independent source: the terminal talks to Alpaca directly
over HTTPS on its own thread, so a slow HTTP round-trip never stalls the crypto
path or the GUI. Swapping either data source is isolated to one class
(`BaseFeedHandler` subclass for crypto venues, `AlpacaClient`/`StockFeed` for
stocks).

> Earlier versions routed crypto through a KDB+ tickerplant + RDB pair. That was
> removed: the app's entire use of it was a per-symbol last-value lookup and the
> last 300 trade prices of one symbol, which is a hash map and a ring buffer. See
> `designReview.md` for the measurements and `simplificationPlan.md` for the
> migration.

For the system design -- component map, data flow, the feed wire format, the
threading model, and what is built vs. planned -- see `architecture.md`.

---

## Install (macOS)

Grab the `.dmg`, drag the app to Applications, done. It bundles its own
feedhandler and Python runtime -- nothing to install, no Homebrew, no license.
Crypto streams as soon as you click START DESK.

To build one yourself:

```bash
.venv/bin/python -m pip install -r feedhandler/requirements-dev.txt  # PyInstaller
scripts/make_dmg.sh 1.2.0        # -> dist/Execution-Layer-Terminal-1.2.0.dmg
EL_SKIP_FEED=1 scripts/make_dmg.sh   # terminal only, much faster
```

The signature is ad-hoc, so a downloaded copy needs right-click -> Open the first
time (or `xattr -dr com.apple.quarantine`). For wider distribution, sign with a
Developer ID via `CODESIGN_ID=... scripts/make_dmg.sh` and notarize.

Optional, for the Stocks tab: put your Alpaca keys in `~/.execution-layer.env`,
which the app sources at launch (Finder gives it no shell environment):

```
ALPACA_API_KEY_ID=...
ALPACA_API_SECRET_KEY=...
```

---

## Prerequisites (building from source)

- macOS on Apple Silicon. **The GUI is macOS-only** as it stands: `cpp/Makefile`
  links `-framework OpenGL/Cocoa/IOKit/CoreVideo` and resolves GLFW via Homebrew.
  The headless targets build anywhere; porting the GUI to Linux means an X11/
  Wayland GLFW backend and dropping the framework flags. Untested there.
- Python 3.11 (what the feedhandler is developed and tested against; nothing in
  it should require 3.11 specifically, but newer versions are untested)
- A C++20 compiler (Apple clang works)
- For the GUI: GLFW, plus the vendored Dear ImGui, ImPlot, and nlohmann/json (fetched below)
- libcurl (ships with macOS/most Linux distros) -- for the stock feed
- Optional, for stocks: a free [Alpaca](https://alpaca.markets) account and API
  key/secret. Without it the terminal runs exactly as before, crypto-only.

---

## How to run

The system runs as two processes: the feedhandler and the terminal. They can be
started in either order.

### 1. One-time setup

```bash
cd execution-layer

# Python environment for the feedhandler
python3 -m venv .venv
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

# Optional: enables the Stocks tab (Alpaca free market-data plan). Without
# these the terminal runs crypto-only, same as before.
export ALPACA_API_KEY_ID="..."
export ALPACA_API_SECRET_KEY="..."
```

### 2. Start the feed (terminal 1)

```bash
cd execution-layer
scripts/run_stack.sh coinbase
```

With no symbol argument this boots the top-crypto universe (~95 live USD
products), so the terminal is populated out of the box. To stream a specific set
instead:

```bash
scripts/run_stack.sh coinbase BTC-USD,ETH-USD
```

Wait until you see `[coinbase] connected + subscribed to N products`. Until you
click START DESK in the terminal there is nothing listening, so the feedhandler
will report that it's dropping ticks and retrying -- that's expected.

### 3. Build and run the terminal (terminal 2)

```bash
cd execution-layer/cpp
make gui
./build/terminal
```

Optional argument: `./build/terminal <feed-port>` (default `5020`).

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

To stop: Ctrl-C in terminal 1, then close the terminal window.

---

## Running without a GUI / display

```bash
cd execution-layer/cpp
make run-selftest    # offline, no network: asserts the OMS/risk/PnL pipeline
make engine-test     # threaded engine + command/view handoff, needs a feedhandler
```

`selftest` is self-contained and exits non-zero on failure. `engine-test` wants a
feedhandler running alongside it (`scripts/run_stack.sh mock` is enough).

---

## Project layout

```
execution-layer/
  feedhandler/             Python market-data feedhandler
    schema.py              normalized Trade / Quote types
    base.py                buffering, flush loop, TLS, control loop (abstract)
    coinbase.py            Coinbase WebSocket + catalog + dynamic subscribe
    binance.py             Binance WebSocket implementation
    mock.py                synthetic feed (no network) for offline testing
    publisher.py           NDJSON batches -> the terminal's feed socket
    universe.py            default top-crypto universe subscribed at boot
    __main__.py            CLI entrypoint (--venue coinbase|binance|mock)
  cpp/                     C++ execution terminal
    include/execution/     types, portfolio, risk, oms, matching, market_store,
                            feed_server, alpaca_client, stock_feed, engine
    src/                   implementations + terminal_gui.cpp + headless tests
    Makefile               build (make gui / make all / make alpaca-test)
    README.md              terminal design + build detail
  scripts/run_stack.sh     launch the feedhandler
  scripts/make_dmg.sh      package terminal + feedhandler as a macOS .app + .dmg
  scripts/make_icon.py     generate assets/icon.icns (stdlib only)
  scripts/feedhandler_entry.py  PyInstaller entry point for the frozen feed
  architecture.md          system design, data flow, and rationale
  designReview.md          objective review of the architecture
  simplificationPlan.md    the KDB+ removal plan this repo followed
```

---

## What is verified

Tested end to end on Apple Silicon (macOS 26, Python 3.11, Apple clang):

- Coinbase live -> feedhandler -> terminal: real quotes, 407-product catalog.
- Buying/selling on live prices with correct cash, position, and PnL accounting;
  `engine-test` buys, marks the position as the market moves, then flattens.
- Control plane: requesting a new ticker at runtime dynamically subscribes the
  feed and it starts streaming, no restart.
- Feedhandler restart / terminal restart in either order, with automatic
  reconnect in both directions.
- `selftest` asserts the OMS state machine, risk gates, kill switch, and
  round-trip PnL arithmetic offline.
- Parsing unit tests for Binance and Coinbase wire formats.

---

## Known gotchas

- The feedhandler will say it's "dropping ticks" until you click START DESK --
  the terminal only opens its feed socket once the desk is live. It reconnects
  on its own; nothing to restart.
- If the terminal logs `bind() failed on port 5020`, another copy is already
  running.
- TLS behind an intercepting proxy: if the feed gets CERTIFICATE_VERIFY_FAILED,
  the feedhandler uses certifi by default; for local debugging only you can set
  EL_INSECURE_SSL=1 (never for order routing).
- Binance is geo-blocked in some regions (HTTP 451). Use Coinbase there.

---

## Roadmap

Done: live feedhandler (Binance + Coinbase); in-process market store; threaded
engine with paper OMS, risk gate, and matching; docked Bloomberg-style GUI with
account bar, tabbed market watch (crypto + stocks), ticker search, order ticket,
tabbed positions (current/previous), event log, and live price + PnL charts;
runtime control plane to add any crypto ticker; top-crypto boot universe; stocks
via Alpaca's free real-time IEX data API on a dedicated polling thread; macOS
.app/.dmg packaging.

Next: candlestick / OHLC charts with historical backfill; durable tick history
(Parquet + DuckDB) to replace the in-memory-only store; an analysis layer
producing signals for the engine to consume; optional live order routing to an
exchange testnet behind the risk gate.
