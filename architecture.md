# Architecture

This document describes the whole system and the reasoning behind it.

What is **built and running today**: the market-data pipeline (feedhandler ->
terminal), the in-process market store, the C++ execution terminal (paper OMS,
risk gate, paper matching, threaded engine), the docked ImGui/ImPlot GUI, a
runtime control plane for adding tickers on the fly, and paper stock trading via
Alpaca alongside crypto via Coinbase.

What is **still planned** (and marked as such below): the analysis/signal layer,
durable tick history, and any live order routing. Section 4 and the "Planned"
rows in Sec. 2 describe intended end state, not current code.

> **History.** Crypto market data used to flow through a KDB+ tickerplant + RDB
> pair. That layer was removed once measured: the application's entire use of it
> was a per-symbol last-value lookup and the last 300 trade prices of the charted
> symbol, and the `quote` table it accumulated forever was never read at all.
> `designReview.md` has the numbers; `simplificationPlan.md` has the migration.

## 1. Design goals

1. **Keep the hot path in-process.** Quotes and recent history live in the same
   process that trades on them, so reading them is a map lookup rather than a
   network round trip. Ingestion stays in Python (where WebSocket and JSON work
   is cheap) and hands off over one localhost socket -- a single boundary
   crossing instead of four.
2. **Source-agnostic ingestion.** The data *source* (which exchange) is swappable
   without touching anything downstream, via the feedhandler abstraction
   (`BaseFeedHandler`). Coinbase is the live venue; a Binance implementation and a
   networkless mock feed are siblings under the same contract.
3. **Store only what is read.** The store keeps a last-value cache and a bounded
   per-symbol price history, because that is exactly what the terminal consumes.
   Durable history is a separate, later concern with a different tool (Sec. 4)
   rather than an unbounded in-memory tape nothing queries.
4. **Analysis is a planned, separate layer.** See Sec. 4. Not implemented yet.
5. **Safety before speed in execution.** The execution layer is a paper OMS with
   risk controls and a paper matching engine *first*; live order routing is last
   and, today, unbuilt.

## 2. Component map

| Component | Language | Port | Status | Role |
|---|---|---|---|---|
| `feedhandler/` | Python | -- | Built | Normalize a crypto exchange stream (Coinbase live; Binance/mock also implemented) -> publish to the terminal |
| `FeedServer` | C++ | 5020 | Built | Localhost socket the feedhandler publishes into; decodes NDJSON batches |
| `MarketStore` | C++ | -- | Built | In-process last-value cache + bounded per-symbol price history + catalog |
| Execution core (`cpp/`) | C++ | -- | Built | OMS state machine, risk gate + kill switch, paper matching |
| `AlpacaClient` / `StockFeed` | C++ | -- | Built | libcurl REST client + background poller for Alpaca (IEX) stock quotes/bars |
| `TradingEngine` | C++ | -- | Built | Threaded desk: reads quotes, runs OMS, republishes an immutable view |
| Terminal GUI | C++ (ImGui/ImPlot) | -- | Built | Bloomberg-style docked dashboard + live charts |
| Analysis layer | Python | -- | Planned | Rolling metrics/ML -> signals |
| Durable history | -- | -- | Planned | Parquet + DuckDB tick archive |

## 3. Data flow

1. A **feedhandler** connects to an exchange WebSocket (Coinbase today) and
   receives raw trade/quote messages.
2. Each message is normalized into a neutral `Trade` / `Quote` dataclass
   (`feedhandler/schema.py`) -- same shape regardless of venue.
3. Ticks are **buffered** and **flushed** on a timer (default 250 ms,
   `EL_FLUSH_INTERVAL`) or when the buffer fills (default 1000, `EL_MAX_BUFFER`),
   to amortize IPC cost.
4. The **publisher** (`publisher.py`) serializes each buffered batch as one line
   of JSON and writes it to the terminal's feed socket. If the terminal isn't
   listening yet, ticks are dropped and the connection retried -- the same
   semantics the tickerplant had with no subscribers.
5. The **FeedServer** (`feed_server.cpp`) decodes each line on its own thread and
   writes straight into the **MarketStore**: `on_quote` replaces the symbol's
   last-value entry, `on_trade` pushes onto its bounded price history.
6. The **engine** reads `snapshot(symbols)` and `history(sym, n)` from the store.
   Both are mutex-guarded map lookups in the same process -- no I/O, no
   serialization, no round trip.

### Runtime control plane

The same socket carries a small control plane so the terminal can change what is
being streamed without a restart:

- The terminal posts a new ticker (`MarketStore::add_symbol`); the feedhandler
  polls for it (`{"m":"poll"}`) and live-subscribes to that product on the
  exchange. The store *drains* the requested list on read, so a symbol is never
  handed out twice.
- The feedhandler publishes the product catalog (`{"m":"products"}`) and the set
  of symbols it is actually streaming (`{"m":"universe"}`) so the terminal can
  browse products and auto-populate its watch list at boot.

### The `Trade` / `Quote` schema

The normalized types every venue produces (`feedhandler/schema.py`):

| Field | Notes |
|---|---|
| `event_time_ns` | Exchange-reported time, unix nanoseconds. Full absolute time, not offset-from-midnight -- crypto trades 24/7 across day boundaries |
| `recv_time_ns` | Feedhandler receive time; `recv - event` is ingestion latency |
| `symbol` | Instrument, e.g. `BTC-USD` |
| `exchange` | Source venue; the feedhandler multiplexes several |
| `price` / `size` | `size` is float for fractional crypto quantities |
| `side` | Trade aggressor `B`/`S`/`U`, so imbalance is computable from the tape |
| `bid`/`ask`/`bsize`/`asize` | Top-of-book on `Quote` |

The wire format the publisher sends is narrower than this on purpose: the
terminal consumes top-of-book, trade price, and event time, so those are what
cross the socket. `exchange`, `size`, and `side` stay in the Python dataclasses
for the analysis layer (Sec. 4) rather than being serialized into a store that
would never read them -- which is exactly what the previous KDB+ schema did with
nine of its fifteen columns.

## 4. Two speeds of analysis (planned, not yet built)

This is the intended analytics design; none of it exists in the repo today. The
split is by cost:

- **Continuous, cheap, in C++** -- VWAP, order-flow imbalance, moving-average
  momentum, rolling volatility. These are incremental over the tick stream and
  belong next to the store, computed as ticks arrive.
- **Periodic, expensive, in Python** -- pairwise regression / beta, Engle-Granger
  cointegration, skew/kurtosis. Run on a timer over a lookback window, which
  needs history the in-memory store deliberately doesn't keep. The intended shape
  is a Parquet tick archive queried with DuckDB: embedded, columnar, no server,
  no license, and it has the `ASOF JOIN` this workload actually wants.

The C++ side already has the receiving end stubbed: a `Signal` value type
(`types.hpp`) that the OMS can turn into an order. What is missing is the engine
that produces signals and the history to compute them over.

## 5. Execution layer (built)

- **Transport:** none in the hot path. Crypto quotes and history come from
  `MarketStore` in the same process; stocks come from `StockFeed`'s cache, also
  in-process. Both are filled by their own threads, so the engine never does I/O.
- **Threading:** one engine thread owns the trading state (`Portfolio`, `OMS`,
  quote cache). It reads the two market-data caches on a fast loop (~30 ms),
  drains a command queue, and republishes an immutable `EngineView`. The GUI
  thread only reads that snapshot and posts `Command`s; two mutexes guard the
  queue and the view, and the market-data caches guard themselves.
- **Order lifecycle:** a state machine (`New -> Sent -> PartiallyFilled -> Filled
  / Cancelled / Rejected`) with legal transitions enforced in the OMS
  (`oms.cpp`), and unique, monotonically increasing order IDs.
- **Gates in front of every order:** (1) account business rules -- buying-power
  check and long-only spot (no shorting); (2) risk limits -- max order quantity,
  max position quantity, max order notional -- plus a kill switch that, once
  tripped, denies every subsequent order. Fail-closed (`risk.cpp`). There is no
  rate limiter today.
- **Matching:** a paper matching engine (`matching.cpp`) fills market orders
  against the live top-of-book and only fills a limit order if it is marketable
  against the touch. This is the safety net; no real routing exists.
- **Stocks (Alpaca):** `AlpacaClient` is a libcurl REST client against Alpaca's
  free "Basic" plan (real IEX-venue real-time quotes + historical daily bars, no
  order routing). `StockFeed` polls it on its own background thread (HTTP is too
  slow for the engine loop), refreshing each watched symbol at most every ~10 s.
  Stocks are off unless `ALPACA_API_KEY_ID` / `ALPACA_API_SECRET_KEY` are set; the
  OMS, risk manager, and matching engine are asset-agnostic and treat stocks and
  crypto identically.
- **Strategy input:** today, orders come from the GUI order ticket (manual
  buy/sell/flatten) and, programmatically, from `Signal`s in the headless tests.
  A declarative strategy engine (specs interpreted by a rules engine, later
  embedded scripting) is a design idea, not implemented.
- **GUI:** Dear ImGui + ImPlot, themed as a Bloomberg-style terminal -- account
  bar with kill switch, Market Watch (Crypto + Stocks tabs), Ticker Search over
  the Coinbase catalog, live price chart, account PnL chart, order ticket, and
  Positions (Current / Previous round-trips) with an event log.

## 6. Deliberately deferred

- **Analysis/signal layer** (Sec. 4).
- **Durable tick history** -- the store is memory-only and bounded per symbol;
  data lives for the life of the process. Sec. 4 sketches the intended shape.
- **Replay log** for disaster recovery.
- **Live order routing** to any venue (crypto or stock) -- everything is paper.
- **Authentication** for order routing and for Coinbase's authenticated
  market-data channels (only public channels are used).
- **Backpressure / drop policy** if the terminal can't keep up. Today the socket
  buffer plus the feedhandler's own flush batching absorb it, and a stalled
  terminal shows as a disconnect rather than unbounded growth.

## 7. Why this shape

The system is shaped by where the data actually has to be. Execution reads a
handful of symbols' top-of-book tens of times a second and needs the answer
immediately, so that state lives in the process that trades on it -- a map
lookup, not a query. Ingestion is I/O-bound protocol work over WebSockets and
JSON, which is cheap in Python and tedious in C++, so it stays in Python and
crosses one boundary.

That boundary is one localhost socket carrying batched ticks, and it is the only
serialization step in the pipeline. The previous design crossed four
representation changes (dataclass -> PyKX vector -> q table -> hand-parsed C++
struct) to deliver the same `Quote` to the same OMS that the Alpaca stock path
already reached with a single JSON parse and no database at all. The stock path
was the argument; this is the crypto path agreeing with it.

What the system gives up is a real analytical engine over history. That is a
genuine loss and a deliberate one: nothing in the terminal queried history, the
tape was accumulating unread, and the right tool for the analysis layer when it
arrives is an embedded columnar engine over a Parquet archive (Sec. 4) rather
than an always-on server holding everything in RAM.
