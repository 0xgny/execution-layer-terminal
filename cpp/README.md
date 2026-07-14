# Execution Layer -- C++ terminal

The execution half of the platform: a paper-trading terminal that trades on live
Coinbase market data flowing through KDB+. It turns user actions into orders,
gates them through risk controls, fills them on a paper matching engine, and
shows real-time PnL -- rendered as a Bloomberg-terminal-style docked GUI
(Dear ImGui + ImPlot). Paper only; no real orders are routed.

## How data reaches C++

The terminal is a native KDB+ client. Rather than depend on KX's prebuilt `c.o`
(not published for arm64 macOS) or embed the whole `libq` runtime, it implements
the small q IPC protocol directly over a socket (`kdb_client.cpp`, verified
byte-for-byte against a live q). It calls the RDB's `snap[syms]` and `hist[]`
functions and parses the results. Zero external SDK.

```
Coinbase -> feedhandler(Py) -> tp.q -> rdb.q --(q IPC over socket)--> KdbClient -> TradingEngine -> GUI
```

## Threading model

- One engine thread (`TradingEngine`) owns all mutable state (`KdbClient`,
  `Portfolio`, `OMS`, quote cache) and does all KDB+ IPC (client state is not
  thread-safe, so it lives on exactly one thread). It polls quotes ~33x/s,
  processes queued commands, and republishes an immutable `EngineView`.
- The GUI thread never touches trading state: it reads an `EngineView` snapshot
  each frame and posts `Command`s (buy/sell/flatten/add-symbol/kill). Two small
  mutexes guard the command queue and the published view; the render loop never
  blocks on IPC.

## Build and run

### 1. One-time setup

```bash
brew install glfw
cd cpp/third_party
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git
git clone --depth 1 https://github.com/epezent/implot.git
```

### 2. Bring up the data stack (from the project root)

```bash
scripts/run_stack.sh coinbase          # boots the ~95-symbol top-crypto universe
# or a specific set:
scripts/run_stack.sh coinbase BTC-USD,ETH-USD
```

### 3. Build and run

```bash
cd cpp
make gui                 # build the terminal (needs glfw + vendored imgui/implot)
./build/terminal         # opens the Bloomberg-style window
# optional args:  ./build/terminal <rdb-host> <rdb-port>   (default 127.0.0.1 5011)
```

On launch, enter your initial capital, hit START DESK, and you get a tiled desk:

- Account bar -- cash / equity / live PnL / connection status / KILL switch
- Market Watch -- live top-of-book per symbol; click a row to chart it
- Ticker Search -- search/browse the full Coinbase catalog (~400 USD products);
  click one and the control plane subscribes the feed to it live and charts it
- Price Chart (ImPlot) -- live last-trade price of the focused symbol (amber)
- PnL (ImPlot) -- account PnL over time; green while in profit, red while in loss
- Order Ticket -- pick a symbol, enter a dollar amount, BUY / SELL / FLATTEN
- Positions -- open positions with live unrealized PnL and per-row flatten
- Event Log -- fills, rejects, connection events

### The control plane

Selecting a new ticker posts an `AddSymbol` command, the engine calls `.u.addsym`
on the tickerplant, the Python feedhandler polls that list and sends a live
subscribe to Coinbase for the new product, its ticks flow into the RDB, and the
terminal charts and trades it. All at runtime, no restart. The product catalog is
fetched once by the feedhandler (Coinbase REST) and published into the tickerplant
for the terminal to browse.

By default the Coinbase feed boots the top-crypto universe (~95 live USD products;
see `feedhandler/universe.py`), so Market Watch is populated out of the box. The
engine auto-discovers them from the tickerplant's `universe` list.

Note: the vendored ImPlot is `master`; line colors are set via
`ImPlotSpec::LineColor` (the older `SetNextLineStyle`/`ImPlotCol_Line` are gone).

## Headless targets (no display / no GLFW needed)

```bash
make all              # builds selftest and engine_test
make run-selftest     # single-threaded live buy / PnL / flatten against the RDB
make engine-test      # exercises the threaded engine + command/view handoff
```

## Layout

| File | Role |
|---|---|
| `include/execution/types.hpp` | `Side`, `Order`, `Fill`, `Signal`, `Quote`, `Position` (+ PnL math) |
| `include/execution/portfolio.hpp` | cash + positions + equity/PnL accounting |
| `risk.hpp` / `risk.cpp` | `RiskManager`: limits + kill switch (fail-closed) |
| `matching.hpp` / `matching.cpp` | `PaperMatchingEngine`: fills vs top-of-book |
| `oms.hpp` / `oms.cpp` | `OrderManager`: state machine, buying-power / no-short gates, routing |
| `kdb_client.hpp` / `kdb_client.cpp` | pure-socket q IPC client (connect, snap, hist, catalog) |
| `engine.hpp` / `engine.cpp` | `TradingEngine`: background thread, `EngineView`, `Command`s |
| `src/terminal_gui.cpp` | the ImGui/ImPlot terminal |
| `src/selftest.cpp`, `src/engine_test.cpp` | headless live checks |

## What is verified

- `KdbClient` pulls live Coinbase quotes through KDB+ (real bid/ask/sizes).
- `selftest`: funded $10k, bought $3000 of BTC live, PnL tracked the market in real
  time, flattened; cash restored, realized PnL correct.
- `engine_test`: threaded engine buy / view / flatten works; event log correct; the
  engine auto-watches the ~95-symbol universe and parses the 407-product catalog.
- GUI compiles and links clean and runs its render loop (verify visuals on a
  machine with a display).

## Roadmap (this component)

Done: native KDB+ client; threaded engine with paper OMS, risk gate, and matching;
docked Bloomberg-style GUI; runtime control plane to add any ticker; live price and
PnL charts.

Next:
- Candlestick / OHLC charts + historical backfill (Coinbase REST candles).
- Consume the KDB+ `signal` table (signals from the Python Analysis Engine).
- Stocks via a polled Yahoo feed (`exch=yahoo`) into the same pipeline.
- Optional live order routing to an exchange testnet, behind the risk gate.
