# Architecture

This document describes the whole intended system and the reasoning behind it.
Only Phase 1 (the feedhandler + KDB+ tick store) is built today; later phases are
described so the current design choices make sense in context.

## 1. Design goals

1. **One low-latency backbone.** Market data, analytics, and execution all share a
   single KDB+ IPC fabric rather than bolting together unrelated services. This is
   inherited from the q-sim prototype and is the reason Python (research) and C++
   (execution) can cooperate without a translation layer between them.
2. **Source-agnostic ingestion.** The data *source* (which exchange) must be
   swappable without touching anything downstream. Achieved via the feedhandler
   abstraction (`BaseFeedHandler`).
3. **Push compute to the data.** Aggregations that reduce data volume (VWAP,
   as-of joins, moving averages) run in q on the server, not by shipping millions
   of rows to Python. This is the "data gravity" principle from q-sim.
4. **Two speeds of analysis.** Cheap signals run continuously in q; expensive
   models (regression, cointegration) run periodically in Python over rolling
   windows. See §4.
5. **Safety before speed in execution.** The execution layer is an OMS with risk
   controls and a paper matching engine *first*; live order routing is last and
   optional.

## 2. Component map

| Component | Language | Port | Status | Role |
|---|---|---|---|---|
| `kdb/schema.q` | q | — | ✅ | Shared `trade`/`quote` table contract |
| `kdb/tp.q` (tickerplant) | q | 5010 | ✅ | Pub/sub router; no compute, no storage |
| `kdb/rdb.q` (RDB) | q | 5011 | ✅ | In-memory intraday store + query surface |
| `feedhandler/` | Python | — | ✅ | Normalize exchange streams (Binance, Coinbase) → publish to tp |
| Execution core (`cpp/`) | C++ | — | ✅ | OMS state machine, risk gate + kill switch, paper matching |
| `KdbMarketData` (C++ ↔ RDB) | C++ | — | ⬜ Planned | Live quotes into the execution core via the KDB+ C API |
| Analysis Engine | Python | — | ⬜ Planned | Rolling metrics/ML → `signal` table |
| HDB (historical DB) | q | — | ⬜ Planned | On-disk partitioned tick history |
| Terminal GUI | C++ (ImGui) | — | ⬜ Planned | Bloomberg-style live dashboard |

## 3. Data flow (Phase 1, built)

1. A **feedhandler** connects to an exchange WebSocket and receives raw
   trade/quote messages.
2. Each message is normalized into a neutral `Trade` / `Quote` dataclass
   (`feedhandler/schema.py`) — same shape regardless of venue.
3. Ticks are **buffered** and **flushed** on a timer (default 250 ms) or when the
   buffer fills, to amortize IPC cost.
4. The **publisher** (`publisher.py`) turns each buffered batch into typed PyKX
   column vectors and calls `.u.upd[table; data]` on the **tickerplant** over IPC,
   fire-and-forget (`wait=False`).
5. The **tickerplant** (`tp.q`) fans the update out asynchronously to every
   subscriber. It does no computation and stores nothing itself.
6. The **RDB** (`rdb.q`) receives the update and appends it into its in-memory
   `trade`/`quote` tables, where it is immediately queryable on port 5011.

### The `trade` / `quote` schema

The shared contract (`kdb/schema.q`), and how it improves on q-sim's prototype:

| Column | Type | Notes / difference from q-sim |
|---|---|---|
| `time` | timestamp | Full date+ns (q-sim used `timespan`, offset-from-midnight — breaks across days; crypto is 24/7) |
| `recv` | timestamp | Feedhandler receive time; `recv - time` = ingestion latency (new) |
| `sym` | symbol | Instrument, e.g. `` `BTCUSDT `` |
| `exch` | symbol | Source venue (new; enables multi-exchange multiplexing) |
| `price`/`size` | float | `size` is `float` for fractional crypto (q-sim used `long` for whole shares) |
| `side` | char | Trade aggressor `B`/`S`/`U` (new; lets us compute imbalance from the tape) |
| `bid`/`ask`/`bsize`/`asize` | float | Top-of-book on `quote` |

## 4. Two speeds of analysis (Phase 2, planned)

The Stock-Analysis-Engine's calculations split cleanly by cost:

- **Continuous, cheap, in q** — VWAP, order-flow imbalance, moving-average
  momentum, rolling volatility, rolling correlation. These reduce data volume and
  belong on the server. (q-sim already prototyped VWAP, imbalance via `aj`, and MA
  momentum.)
- **Periodic, expensive, in Python** — pairwise regression / β, Engle-Granger
  cointegration, skew/kurtosis. Run on a timer (e.g. every N seconds) over a
  lookback window pulled from the RDB; results are written back into a KDB+
  `signal` table that the execution layer subscribes to.

## 5. Execution layer (Phases 4–7, planned)

- **Transport:** the C++ engine connects to KDB+ via the native C API (`k.h`),
  subscribing to the `signal` table (from analytics) and live prices (from the
  RDB). No Python in the execution hot path.
- **Core:** an order state machine (New → Sent → PartiallyFilled → Filled /
  Cancelled / Rejected) with idempotent order IDs.
- **Risk (in front of every order):** position limits, max order size, max
  notional, rate limiting, and a kill switch. Non-negotiable and built early.
- **Matching:** a paper/simulated matching engine fills orders against the live
  book first — this doubles as the backtester and the safety net — before any real
  routing.
- **Strategy input:** declarative strategy specs (YAML/JSON) interpreted by a
  small rules engine, later augmented with embedded scripting (Lua/Python) so
  users define strategies without recompiling C++.
- **GUI:** Dear ImGui + ImPlot, themed as a Bloomberg-style terminal — live
  blotter, positions, PnL, signal feed, price charts.

## 6. Deliberately deferred

- **Tickerplant replay log** for disaster recovery (standard in production KDB+).
- **HDB + end-of-day flush** — the RDB is memory-only for now.
- **Authentication** for exchange order routing and Coinbase's authenticated
  market-data channels.
- **Backpressure / drop policy** if a subscriber can't keep up (today we rely on
  async sends and generous buffers).

## 7. Why KDB+ / this shape at all

Moving raw ticks over a network into Python to compute aggregates is the classic
anti-pattern this design avoids. KDB+ keeps hot intraday data in memory, columnar,
and queryable with microsecond latency, and its IPC lets heterogeneous clients
(Python research, C++ execution) share the same data without a bespoke bus. The
pub/sub split (tickerplant routes; RDB stores; clients compute) means each concern
scales and fails independently.
```
