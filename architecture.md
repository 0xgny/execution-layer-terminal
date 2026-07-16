# Architecture

This document describes the whole system and the reasoning behind it.

What is **built and running today**: the market-data pipeline (feedhandler ->
tickerplant -> RDB), the C++ execution terminal (paper OMS, risk gate, paper
matching, threaded engine), the docked ImGui/ImPlot GUI, a runtime control plane
for adding tickers on the fly, and paper stock trading via Alpaca alongside
crypto via Coinbase.

What is **still planned** (and marked as such below): the analysis/signal layer
(rolling metrics and models writing a KDB+ `signal` table), on-disk history
(HDB), and any live order routing. Sections 4 and the "Planned" rows in Sec. 2
describe intended end state, not current code.

## 1. Design goals

1. **One low-latency backbone.** Market data and execution share a single KDB+
   IPC fabric rather than bolting together unrelated services. This is inherited
   from the q-sim prototype and is what lets Python (ingestion/research) and C++
   (execution) cooperate over the same data without a translation layer. (The
   analytics half of this vision is not built yet -- see Sec. 4.)
2. **Source-agnostic ingestion.** The data *source* (which exchange) is swappable
   without touching anything downstream, via the feedhandler abstraction
   (`BaseFeedHandler`). Coinbase is the live venue; a Binance implementation and a
   networkless mock feed are siblings under the same contract.
3. **Push compute to the data.** The design intent is that volume-reducing
   aggregations (VWAP, as-of joins, moving averages) run in q on the server, not
   by shipping millions of rows to Python. Today the RDB only serves last-value
   snapshots and recent history (Sec. 3); the server-side analytics are planned.
4. **Two speeds of analysis (planned).** Cheap signals in q, expensive models in
   Python -- see Sec. 4. Neither is implemented yet.
5. **Safety before speed in execution.** The execution layer is a paper OMS with
   risk controls and a paper matching engine *first*; live order routing is last
   and, today, unbuilt.

## 2. Component map

| Component | Language | Port | Status | Role |
|---|---|---|---|---|
| `kdb/schema.q` | q | -- | Built | Shared `trade`/`quote` table contract |
| `kdb/tp.q` (tickerplant) | q | 5010 | Built | Pub/sub router + control plane; no compute, no storage |
| `kdb/rdb.q` (RDB) | q | 5011 | Built | In-memory intraday store + last-value caches + query surface |
| `feedhandler/` | Python | -- | Built | Normalize a crypto exchange stream (Coinbase live; Binance/mock also implemented) -> publish to tp |
| Execution core (`cpp/`) | C++ | -- | Built | OMS state machine, risk gate + kill switch, paper matching |
| `KdbClient` (C++ -> KDB+) | C++ | -- | Built | Live quotes/history via a pure-socket q IPC client (no external SDK) |
| `AlpacaClient` / `StockFeed` | C++ | -- | Built | libcurl REST client + background poller for Alpaca (IEX) stock quotes/bars |
| `TradingEngine` | C++ | -- | Built | Threaded desk: polls quotes, runs OMS, republishes an immutable view |
| Terminal GUI | C++ (ImGui/ImPlot) | -- | Built | Bloomberg-style docked dashboard + live charts |
| Analysis Engine | Python | -- | Planned | Rolling metrics/ML -> `signal` table |
| HDB (historical DB) | q | -- | Planned | On-disk partitioned tick history |

## 3. Data flow

1. A **feedhandler** connects to an exchange WebSocket (Coinbase today) and
   receives raw trade/quote messages.
2. Each message is normalized into a neutral `Trade` / `Quote` dataclass
   (`feedhandler/schema.py`) -- same shape regardless of venue.
3. Ticks are **buffered** and **flushed** on a timer (default 250 ms,
   `EL_FLUSH_INTERVAL`) or when the buffer fills (default 1000, `EL_MAX_BUFFER`),
   to amortize IPC cost.
4. The **publisher** (`publisher.py`) turns each buffered batch into typed PyKX
   column vectors and calls `.u.upd[table; data]` on the **tickerplant** over IPC,
   fire-and-forget (`wait=False`).
5. The **tickerplant** (`tp.q`) fans the update out asynchronously (`neg h`) to
   every subscriber. It does no computation and stores nothing itself.
6. The **RDB** (`rdb.q`) receives the update, appends it into its in-memory
   `trade`/`quote` tables, and refreshes per-symbol last-value caches. It exposes
   `snap[syms]` (latest top-of-book + last trade), `hist[sym;n]` (recent trade
   prices for charting), and `counts[]` (row counts + mean ingestion latency) on
   port 5011. It computes no analytics beyond these.

### Runtime control plane

The tickerplant also carries a small control plane so the terminal can change
what is being streamed without a restart:

- The terminal posts a new ticker (`.u.addsym`); the feedhandler polls the
  `requested` list and live-subscribes to that product on the exchange.
- The feedhandler publishes the product catalog (`.u.setproducts`) and the set of
  symbols it is actually streaming (`.u.setuniverse`) so the terminal can browse
  products and auto-populate its watch list at boot.

### The `trade` / `quote` schema

The shared contract (`kdb/schema.q`), and how it improves on q-sim's prototype:

| Column | Type | Notes / difference from q-sim |
|---|---|---|
| `time` | timestamp | Full date+ns (q-sim used `timespan`, offset-from-midnight -- breaks across days; crypto is 24/7) |
| `recv` | timestamp | Feedhandler receive time; `recv - time` = ingestion latency (new) |
| `sym` | symbol | Instrument, e.g. `` `BTC-USD `` |
| `exch` | symbol | Source venue (new; enables multi-exchange multiplexing) |
| `price`/`size` | float | `size` is `float` for fractional crypto (q-sim used `long` for whole shares) |
| `side` | char | Trade aggressor `B`/`S`/`U` (new; lets us compute imbalance from the tape) |
| `bid`/`ask`/`bsize`/`asize` | float | Top-of-book on `quote` |

## 4. Two speeds of analysis (planned, not yet built)

This is the intended analytics design; none of it exists in the repo today. The
split is by cost:

- **Continuous, cheap, in q** -- VWAP, order-flow imbalance, moving-average
  momentum, rolling volatility, rolling correlation. These reduce data volume and
  belong on the server.
- **Periodic, expensive, in Python** -- pairwise regression / beta, Engle-Granger
  cointegration, skew/kurtosis. Run on a timer over a lookback window pulled from
  the RDB; results written into a KDB+ `signal` table that the execution layer
  subscribes to.

The C++ side already has the receiving end stubbed: a `Signal` value type
(`types.hpp`) that the OMS can turn into an order. What is missing is the Python
engine that produces signals and the `signal` table itself.

## 5. Execution layer (built)

- **Transport:** the C++ engine connects to KDB+ via a small pure-socket q IPC
  client (`kdb_client.cpp`; no external SDK -- KX's `c.o` is not published for
  arm64 macOS), reading live crypto prices/history from the RDB. No Python in the
  execution hot path. Stocks come from Alpaca over HTTPS on a separate thread (see
  below), not through KDB+.
- **Threading:** one engine thread owns all mutable state (`KdbClient`,
  `Portfolio`, `OMS`, quote cache) and does all KDB+ IPC (client state is
  single-threaded). It polls quotes on a fast loop (~30 ms), drains a command
  queue, and republishes an immutable `EngineView`. The GUI thread only reads that
  snapshot and posts `Command`s; two mutexes guard the queue and the view.
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

- **Analysis/signal layer and the `signal` table** (Sec. 4).
- **HDB + end-of-day flush** -- the RDB is memory-only; data lives for the life of
  the process.
- **Tickerplant replay log** for disaster recovery (standard in production KDB+).
- **Live order routing** to any venue (crypto or stock) -- everything is paper.
- **Authentication** for order routing and for Coinbase's authenticated
  market-data channels (only public channels are used).
- **Backpressure / drop policy** if a subscriber can't keep up (today we rely on
  async sends and generous buffers).

## 7. Why KDB+ / this shape at all

Moving raw ticks over a network into Python to compute aggregates is the classic
anti-pattern this design avoids. KDB+ keeps hot intraday data in memory, columnar,
and queryable with microsecond latency, and its IPC lets heterogeneous clients
(Python ingestion, C++ execution) share the same data without a bespoke bus. The
pub/sub split (tickerplant routes; RDB stores; clients read) means each concern
scales and fails independently -- and leaves a clean seam for the planned
analytics layer to plug into.
