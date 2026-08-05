# Execution Layer -- C++ terminal

The execution half of the platform: a paper-trading terminal that trades on live
Coinbase market data, plus stocks via Alpaca's free
market-data API. It turns user actions into orders, gates them through risk
controls, fills them on a paper matching engine, and shows real-time PnL --
rendered as a Bloomberg-terminal-style docked GUI (Dear ImGui + ImPlot). Paper
only; no real orders are routed to either venue.

## How data reaches C++

**Crypto.** The terminal stores its own market data. `FeedServer` listens on
127.0.0.1:5020, the Python feedhandler connects and writes batched ticks as
newline-delimited JSON, and `MarketStore` keeps a per-symbol last-value cache
plus a bounded price history. The engine reads both with a map lookup -- same
process, no serialization, no round trip.

```
Coinbase -> feedhandler(Py) --(NDJSON over localhost)--> FeedServer -> MarketStore -> TradingEngine -> GUI
```

This replaced a KDB+ tickerplant + RDB pair. See `../designReview.md` for why.

**Stocks.** `AlpacaClient` (`alpaca_client.hpp`/`.cpp`) is a small libcurl-based
REST client against Alpaca's free "Basic" market-data plan: real IEX-venue
real-time quotes and historical daily bars, no live order routing (this app has
its own paper OMS already). `StockFeed` owns a dedicated background thread that
polls it -- HTTP round-trips are too slow for the engine's 30ms loop, so stock
polling never blocks crypto polling or the GUI.

```
Alpaca (IEX real-time) --(HTTPS)--> AlpacaClient --(own thread)--> StockFeed -> TradingEngine -> GUI
```

## Threading model

- One engine thread (`TradingEngine`) owns the trading state (`Portfolio`,
  `OMS`, quote cache). It reads the market-data caches ~33x/s, processes queued
  commands, and republishes an immutable `EngineView`. It never does I/O.
- `FeedServer` owns a thread that accepts the feedhandler and decodes its
  batches into `MarketStore`, which guards itself with a mutex.
- `StockFeed` owns a second background thread purely for Alpaca HTTP calls,
  round-robining watched stock symbols so the engine only ever does a cheap
  mutex-guarded copy of its cache -- never a network call.
- The GUI thread never touches trading state: it reads an `EngineView` snapshot
  each frame and posts `Command`s (buy/sell/flatten/add-symbol/kill). Two small
  mutexes guard the command queue and the published view; the render loop never
  blocks on anything.

## Build and run

### 1. One-time setup

```bash
brew install glfw
cd cpp/third_party
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git
git clone --depth 1 https://github.com/epezent/implot.git
mkdir -p json && curl -fsSL -o json/json.hpp \
  https://github.com/nlohmann/json/releases/latest/download/json.hpp
```

`libcurl` (used for the Alpaca stock feed) ships with macOS and most Linux
distros already -- nothing extra to install there.

### 2. (Optional) enable stocks

Stocks are off unless both env vars are set; without them the app runs exactly
as before, crypto-only, with the Stocks tab showing a one-line hint instead of
data:

```bash
export ALPACA_API_KEY_ID=...       # free account at alpaca.markets
export ALPACA_API_SECRET_KEY=...
```

Alpaca's free "Basic" data plan gives real IEX-venue real-time quotes (not
delayed) plus historical daily bars, rate-limited to 200 requests/minute --
`StockFeed` refreshes each watched symbol at most once every 10s, so this is
never close to the limit even with dozens of symbols watched.

### 3. Bring up the crypto data stack (from the project root)

```bash
scripts/run.sh coinbase               # boots the ~95-symbol top-crypto universe
# or a specific set:
scripts/run.sh coinbase BTC-USD,ETH-USD
```

### 4. Build and run

```bash
cd cpp
make gui                 # build the terminal (needs glfw + vendored imgui/implot/json)
./build/terminal         # opens the Bloomberg-style window
# optional arg:   ./build/terminal <feed-port>   (default 5020)
```

On launch, enter your initial capital, hit START DESK, and you get a tiled desk:

- Account bar -- cash / equity / live PnL / connection status / KILL switch
- Market Watch -- tabbed **Crypto** (live top-of-book via Coinbase) and
  **Stocks** (Alpaca/IEX real-time); click a row to chart it, or type an exact
  ticker to add one ("+ Watch" / "+ Add Stock")
- Ticker Search -- search/browse the full Coinbase catalog (~400 USD products,
  crypto only -- Alpaca's free plan has no bulk stock catalog); click one and
  the control plane subscribes the feed to it live and charts it
- Price Chart (ImPlot) -- live last-trade price of the focused symbol (amber)
- PnL (ImPlot) -- account PnL over time; green while in profit, red while in loss
- Order Ticket -- pick a symbol (always mirrors whatever's focused elsewhere
  -- Market Watch, Ticker Search, Positions), `$` notional or exact `Qty`
  mode, BUY / SELL / **SELL ALL**. Live bid/ask and your current holding (if
  any) show before you commit; buttons gray out with a tooltip explaining
  why instead of silently rejecting. Clicking an open position (below) jumps
  here pre-filled at that position's exact size so you can top up or exit in
  one motion.
- Positions -- tabbed **Current** (open positions, live unrealized PnL,
  click-to-ticket, per-row flatten) and **Previous** (closed round trips: entry,
  exit, realized PnL) -- a position moves from Current to Previous the moment it
  nets back to flat
- Event Log -- fills, rejects, connection events

### The control plane

Selecting a new ticker posts an `AddSymbol` command, the engine calls `.u.addsym`
in the store, the Python feedhandler polls for it and sends a live
subscribe to Coinbase for the new product, its ticks flow into the store, and the
terminal charts and trades it. All at runtime, no restart. The product catalog is
fetched once by the feedhandler (Coinbase REST) and published into the store
for the terminal to browse.

By default the Coinbase feed boots the top-crypto universe (~95 live USD products;
see `feedhandler/universe.py`), so Market Watch is populated out of the box. The
engine auto-discovers them from the store's `universe` list.

Note: the vendored ImPlot is `master`; line colors are set via
`ImPlotSpec::LineColor` (the older `SetNextLineStyle`/`ImPlotCol_Line` are gone).

## Headless targets (no display / no GLFW needed)

```bash
make all              # builds selftest and engine_test
make run-selftest     # offline: asserts the OMS / risk / PnL pipeline
make engine-test      # exercises the threaded engine + command/view handoff
make alpaca-test      # verifies Alpaca auth + quote/bars parsing (needs env vars set)
```

## Layout

| File | Role |
|---|---|
| `include/execution/types.hpp` | `Side`, `Order`, `Fill`, `Signal`, `Quote`, `Position`, `AssetClass` (+ PnL math) |
| `include/execution/portfolio.hpp` | cash + positions + `ClosedPosition` history + equity/PnL accounting |
| `risk.hpp` / `risk.cpp` | `RiskManager`: limits + kill switch (fail-closed) |
| `matching.hpp` / `matching.cpp` | `PaperMatchingEngine`: fills vs top-of-book |
| `oms.hpp` / `oms.cpp` | `OrderManager`: state machine, buying-power / no-short gates, routing |
| `market_store.hpp` / `market_store.cpp` | in-process last-value cache + bounded price history + catalog |
| `feed_server.hpp` / `feed_server.cpp` | localhost NDJSON socket the feedhandler publishes into |
| `feed_process.hpp` / `feed_process.cpp` | spawns the bundled feedhandler in packaged builds (no-op in a dev tree) |
| `alpaca_client.hpp` / `alpaca_client.cpp` | libcurl REST client for Alpaca quotes + daily bars |
| `stock_feed.hpp` / `stock_feed.cpp` | background poller wrapping `AlpacaClient`, own thread |
| `engine.hpp` / `engine.cpp` | `TradingEngine`: background thread, `EngineView`, `Command`s |
| `src/terminal_gui.cpp` | the ImGui/ImPlot terminal |
| `src/selftest.cpp`, `src/engine_test.cpp`, `src/alpaca_test.cpp` | headless live checks |

## What is verified

- `MarketStore` serves live Coinbase quotes (real bid/ask/sizes) to the engine.
- `selftest`: funded $10k, bought $3000 of BTC live, PnL tracked the market in real
  time, flattened; cash restored, realized PnL correct.
- `engine_test`: threaded engine buy / view / flatten works; event log correct; the
  engine auto-watches the ~95-symbol universe and parses the 407-product catalog.
- GUI (crypto + stocks) compiles and links clean and runs its render loop (verify
  visuals on a machine with a display).
- `alpaca-test` exercises the real Alpaca auth + quote/bars parsing path headlessly.

## Roadmap (this component)

Done: in-process market store + feed socket; threaded engine with paper OMS, risk gate, and matching;
docked Bloomberg-style GUI; runtime control plane to add any ticker; live price and
PnL charts; stocks via Alpaca (real-time IEX quotes + daily bars); Current/Previous
position history; click-a-position-to-ticket.

Next:
- Candlestick / OHLC charts + historical backfill (Coinbase REST candles; Alpaca
  bars are already fetched for stocks but only plotted as a close-price line today).
- Consume signals from an analysis layer.
- Optional live order routing to an exchange testnet, behind the risk gate.
