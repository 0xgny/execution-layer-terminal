# Execution Layer — C++ terminal

The execution half of the platform: a **paper-trading terminal** that trades on
**live Coinbase market data** flowing through KDB+. It turns user actions (or
strategy signals) into orders, gates them through risk controls, fills them on a
paper matching engine, and shows **real-time PnL** — rendered as a
Bloomberg-terminal-style docked GUI (Dear ImGui + ImPlot).

> Status: live wiring done and verified (C++ ↔ KDB+ ↔ Coinbase). Threaded engine
> + docked GUI built. Paper only — no real orders are routed.

## How data reaches C++

The terminal is a **native KDB+ client**. Rather than depend on KX's prebuilt
`c.o` (not published for arm64 macOS) or embed the whole `libq` runtime, we
implement the small q IPC protocol directly over a socket (`kdb_client.cpp`,
verified byte-for-byte against a live q). It calls the RDB's `snap[syms]` function
and parses the returned table into quotes. Zero external SDK.

```
Coinbase ─▶ feedhandler(Py) ─▶ tp.q ─▶ rdb.q ──(q IPC over socket)──▶ KdbClient ─▶ TradingEngine ─▶ GUI
```

## Threading model

- **One engine thread** (`TradingEngine`) owns all mutable state — `KdbClient`,
  `Portfolio`, `OMS`, quote cache — and does all KDB+ IPC (client state isn't
  thread-safe, so it lives on exactly one thread). It polls quotes ~33×/s,
  processes queued commands, and republishes an immutable `EngineView`.
- **The GUI thread** never touches trading state: it reads an `EngineView`
  snapshot each frame and `post()`s `Command`s (buy/sell/flatten/add-symbol/kill).
  Two small mutexes guard the command queue and the published view; the 60 fps
  render loop never blocks on IPC.

## Build & run

### 1. One-time setup

```bash
brew install glfw
cd cpp/third_party
git clone --depth 1 --branch docking https://github.com/ocornut/imgui.git
git clone --depth 1 https://github.com/epezent/implot.git
```

### 2. Bring up the data stack (from the project root)

```bash
scripts/run_stack.sh coinbase BTC-USD,ETH-USD     # feeds the RDB on :5011
```

### 3. Build + run

```bash
cd cpp
make gui                 # build the terminal (needs glfw + vendored imgui/implot)
./build/terminal         # opens the Bloomberg-style window
# optional args:  ./build/terminal <rdb-host> <rdb-port>   (default 127.0.0.1 5011)
```

On launch, enter your **initial capital**, hit **START DESK**, and you get:
- **Account bar** — cash / equity / live PnL / connection status / KILL switch
- **Market Watch** — live top-of-book per symbol; type any Coinbase product
  (e.g. `SOL-USD`) and **+ Watch** to add it at runtime
- **Order Ticket** — pick a symbol, enter a $ amount, **BUY / SELL / FLATTEN**
- **Positions** — open positions with live unrealized PnL and per-row flatten
- **Event Log** — fills, rejects, connection events

## Headless targets (no display / no GLFW needed)

Useful for testing the engine on a server or in CI:

```bash
make all              # execution (demo), selftest, engine_test
make run-selftest     # single-threaded live buy→PnL→flatten against the RDB
make engine-test      # exercises the threaded engine + command/view handoff
./build/execution     # offline MA-cross demo (no network)
```

## Layout

| File | Role |
|---|---|
| `include/execution/types.hpp` | `Side`, `Order`, `Fill`, `Signal`, `Quote`, `Position` (+ PnL math) |
| `include/execution/portfolio.hpp` | cash + positions + equity/PnL accounting |
| `risk.hpp` / `risk.cpp` | `RiskManager`: limits + kill switch (fail-closed) |
| `matching.hpp` / `matching.cpp` | `PaperMatchingEngine`: fills vs top-of-book |
| `oms.hpp` / `oms.cpp` | `OrderManager`: state machine, buying-power / no-short gates, routing |
| `kdb_client.hpp` / `kdb_client.cpp` | pure-socket q IPC client (connect, `snap`, parse) |
| `engine.hpp` / `engine.cpp` | `TradingEngine`: background thread, `EngineView`, `Command`s |
| `strategy.hpp` / `strategy.cpp` | `Strategy` interface + `MovingAverageCross` (demo) |
| `market_data.hpp` / `market_data.cpp` | `MockMarketData` (offline demo source) |
| `src/terminal_gui.cpp` | the ImGui/ImPlot terminal |
| `src/selftest.cpp`, `src/engine_test.cpp`, `src/main.cpp` | headless checks + demo |

## Verified

- `KdbClient` pulls live Coinbase quotes through KDB+ (BTC-USD/ETH-USD, real
  bid/ask/sizes).
- `selftest`: funded $10k, bought $3000 of BTC live, **PnL tracked the market in
  real time**, flattened — cash restored, realized PnL correct.
- `engine_test`: threaded engine buy → view → flatten works; event log correct.
- GUI compiles/links clean and runs its render loop (verify the visuals on a
  machine with a display).

## Roadmap (this component)

1. ✅ Native KDB+ client + threaded engine + paper OMS/risk/matching.
2. ✅ Docked Bloomberg-style GUI: account bar, market watch, order ticket,
   positions, event log; add any ticker at runtime.
3. Control plane so a newly-watched symbol is auto-subscribed on the feed side
   (today the feedhandler must already carry the symbol).
4. ImPlot price charts / sparklines per symbol; per-ticker detail windows.
5. Consume the KDB+ `signal` table (signals from the Python Analysis Engine).
6. Stocks via a polled Yahoo feed (`exch=yahoo`) into the same pipeline.
7. Optional live order routing to an exchange testnet, behind the risk gate.
