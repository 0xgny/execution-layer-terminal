# Execution Layer -- Design Document

This document explains, in depth, how the Execution Layer is built and how every
piece ties together -- from a single trade printing on Coinbase to a green pixel
moving on a PnL chart. It is meant to be read top to bottom by someone who wants
to understand the whole system, including the small details.

Contents:

1. Purpose and philosophy
2. System overview (processes, ports, languages)
3. Technology stack and why each piece was chosen
4. The data model (KDB+ schema and the normalized types)
5. Component deep-dive
   5.1 Feedhandler (Python)
   5.2 Tickerplant, tp.q (q)
   5.3 Real-time database, rdb.q (q)
   5.4 KdbClient (C++ IPC)
   5.5 TradingEngine (C++ threaded core)
   5.6 The terminal GUI (C++ / ImGui / ImPlot)
   5.7 AlpacaClient + StockFeed (C++ stock market data)
6. The q IPC protocol, byte by byte
7. The control plane (catalog, universe, dynamic subscription)
8. Execution logic (OMS state machine, risk, matching, portfolio and PnL)
9. Concurrency and thread-safety
10. End-to-end workflows, traced step by step
11. Failure modes and resilience
12. Key design decisions and tradeoffs
13. Performance characteristics
14. File-by-file reference
15. How to extend the system
16. The web version (a separate, self-contained project)

---

## 1. Purpose and philosophy

The Execution Layer is a real-time crypto **paper-trading terminal**. It streams
live market data from Coinbase, stores it in an in-memory KDB+ tick database, and
presents a Bloomberg-terminal-style desktop application where a user funds a
simulated account and trades it with real-time profit-and-loss, live price and
PnL charts, an order blotter, and a searchable ticker catalog. No real orders are
ever routed; every fill is simulated.

The project fuses two earlier prototypes:

- **q-sim** -- a KDB+/PyKX real-time tick pipeline. This is where the streaming
  backbone (tickerplant + real-time database + a Python feed) comes from.
- **Stock-Analysis-Engine** -- a Python statistics/ML engine. This is the
  research half that will, in a later phase, publish trade signals.

Three principles drive the architecture:

1. **One low-latency backbone.** Market data, execution, and (later) analytics all
   share a single KDB+ IPC fabric rather than a mess of ad-hoc services. Python
   and C++ cooperate by both speaking to KDB+, not by inventing a bespoke bus.
2. **Push compute to the data.** Aggregations that shrink data (last-value
   snapshots, VWAP, moving averages) run in q on the server. We never ship
   millions of rows to a client to compute a number.
3. **Source-agnostic ingestion.** Swapping the exchange is a one-class change in
   Python; nothing downstream (the store, the engine, the GUI) is affected.

And one rule specific to an execution layer:

4. **Safety before speed.** Every order passes a risk gate (limits + a kill
   switch) before it can reach the matching engine. Paper trading comes first;
   any live routing is last and optional.

---

## 2. System overview

The system is four cooperating OS processes:

```
  process 1: q tp.q         Tickerplant       listens on TCP :5010
  process 2: q rdb.q        Real-time DB       listens on TCP :5011, dials :5010
  process 3: python feed    Feedhandler        dials :5010, dials Coinbase (WSS/HTTPS)
  process 4: ./terminal     C++ GUI            dials :5011 and :5010
```

Data flow, end to end:

```
  Coinbase WebSocket (trades + quotes)
        |
        v
  Feedhandler (Python, asyncio)
    - normalize each message to a Trade or Quote
    - buffer, then flush batches over IPC
        |
        |  PyKX .u.upd[table; columns]   (async, port 5010)
        v
  Tickerplant  tp.q  (:5010)
    - pub/sub router: forward each update to every subscriber
    - control plane: requested / products / universe
        |
        |  async .u.upd fan-out to subscribers
        v
  Real-time DB  rdb.q  (:5011)
    - append ticks into in-memory trade / quote tables
    - maintain keyed last-value caches (lastquote / lasttrade)
    - answer snap[syms] and hist[sym;n] queries
        |
        |  q IPC over a plain socket (the C++ client speaks the protocol directly)
        v
  C++ terminal
    KdbClient  ->  TradingEngine (background thread)  ->  ImGui/ImPlot GUI (main thread)
      quotes/       OMS + risk gate + paper matching        panels + charts + order tickets
      history       + portfolio + PnL, published as an
      + catalog     immutable EngineView snapshot
```

The **tickerplant** is the write path's router. The **RDB** is the read model.
The **C++ terminal** is a read client of the RDB (for prices/history) and a
control client of the tickerplant (to add tickers and read the catalog).

Languages: q (the KDB+ language) for the data layer, Python for ingestion, C++20
for the execution core and GUI.

---

## 3. Technology stack and why each piece was chosen

### KDB+ / q (the data layer)

KDB+ is a columnar, in-memory time-series database with an extremely terse
vector language (q) and built-in inter-process communication. It is the standard
tool in finance for exactly this shape of problem: capture high-frequency ticks,
keep them queryable in memory with microsecond latency, and let heterogeneous
clients share the data over IPC. We use three q roles from the classic
"tickerplant architecture":

- a **tickerplant** that only routes (pub/sub),
- a **real-time database** that stores intraday data and answers queries,
- (planned) a **historical database** for on-disk end-of-day data.

Why q and not, say, Postgres or Redis: q's IPC lets a C++ process and a Python
process both talk to the same in-memory tables with no serialization glue of our
own, and q-sql expresses "last value per symbol" or "VWAP" in a few characters
that run server-side. The free community license (kc.lic) is sufficient.

### Python + PyKX + asyncio + websockets (ingestion)

Python is the pragmatic choice for talking to exchange WebSockets and REST APIs.

- **asyncio** drives everything: one event loop runs the socket reader, a
  periodic flush, a stats printer, and the control-plane poller as concurrent
  tasks. Single-threaded cooperative concurrency avoids locks.
- **websockets** is the async WebSocket client for the live streams.
- **PyKX** (KX's official Python-KDB+ bridge) turns Python values into typed q
  column vectors and sends them over IPC with a single call. It is how the
  feedhandler writes into the tickerplant.
- **certifi** provides a current CA bundle so TLS works regardless of the OS
  certificate store (a common macOS gotcha).

A hard constraint: PyKX 4.0 must run on **Python 3.11**, not 3.14 -- see Section 12.

### C++20 (execution core and GUI)

The execution core (order management, risk, matching, PnL) and the GUI are C++
for determinism and control. C++ owns the "hot path" and the desktop rendering.
No exotic libraries are required for the core: it is standard-library C++20.

### Dear ImGui (docking branch) + ImPlot + GLFW + OpenGL (the terminal)

- **Dear ImGui** is an immediate-mode GUI library favored in trading and games
  tooling. Immediate mode means the UI is re-declared every frame from current
  state, which fits a real-time dashboard perfectly: there is no retained widget
  tree to keep in sync with the engine. The **docking** branch adds the tiled,
  dockable panels that give the Bloomberg-terminal feel inside one OS window.
- **ImPlot** is the companion plotting library (live price and PnL charts).
- **GLFW** creates the window and the OpenGL context and delivers input.
- **OpenGL 3.2 core** is the render backend (via ImGui's `imgui_impl_opengl3`).

Why immediate-mode instead of Qt: for a 60 fps data-dense terminal that simply
mirrors engine state, immediate mode is faster to build and has no model/view
synchronization. The whole UI is one function that reads a snapshot and draws.

### The one deliberately hand-rolled piece: a pure-socket q IPC client

The C++ terminal does **not** use KX's C client (`c.o`) or embed the q runtime
(`libq`). Instead it implements just enough of the q IPC wire protocol itself
(Section 6). Reasons: KX does not publish a prebuilt `c.o` for arm64 macOS, and
linking `libq` drags in the entire q interpreter (which takes over the process on
load). A ~250-line protocol client is portable, dependency-free, and was verified
byte-for-byte against a live q. This is the single most unusual engineering
decision in the project and it is what makes the C++ side clean.

### libcurl + nlohmann/json (stock market data)

Stocks are a second, independent asset class layered on later (Section 5.7):
`AlpacaClient` is a small libcurl-based REST client against Alpaca's free
"Basic" market-data plan (real IEX-venue real-time quotes + historical daily
bars), and `nlohmann/json` (single-header, vendored like Dear ImGui) parses
the responses. Unlike the q IPC protocol, there was no reason to hand-roll a
JSON parser -- Alpaca's REST responses are genuinely general-purpose JSON,
not a small fixed binary protocol, so a well-established library was the
right call rather than another from-scratch parser.

---

## 4. The data model

### The shared KDB+ schema (kdb/schema.q)

Two tables are the contract between the feedhandler (writer), the RDB (store), and
the terminal (reader). Both are defined once and loaded by both q processes.

```
trade:([]
    time :`timestamp$();   / exchange event time (date + nanoseconds)
    recv :`timestamp$();   / feedhandler receive time (date + nanoseconds)
    sym  :`symbol$();      / instrument, e.g. `BTC-USD
    exch :`symbol$();      / source venue, e.g. `coinbase
    price:`float$();       / trade price
    size :`float$();       / trade quantity (fractional)
    side :`char$() );      / aggressor: `B`uy / `S`ell / `U`nknown

quote:([]
    time :`timestamp$();   / exchange event time
    recv :`timestamp$();   / feedhandler receive time
    sym  :`symbol$();
    exch :`symbol$();
    bid  :`float$();       / best bid price
    ask  :`float$();       / best ask price
    bsize:`float$();       / size available at best bid
    asize:`float$() );     / size available at best ask
```

Design points, and how they differ from the q-sim prototype:

- **time is a full `timestamp`** (date + nanoseconds), not a `timespan`
  (offset-from-midnight). Crypto trades 24/7 across day boundaries, so the date
  must be kept.
- **recv** is a second timestamp: the wall-clock time our feedhandler received the
  message. `recv - time` is the end-to-end ingestion latency, measurable from
  day one.
- **size is `float`**, because crypto quantities are fractional (0.0123 BTC).
  q-sim used `long` for whole-share equities.
- **exch** tags every row with its venue, enabling several exchanges to
  multiplex into one store.
- **side** on trades stores the aggressor (who crossed the spread), so order-flow
  imbalance can be computed straight from the trade tape.

### The normalized Python types (feedhandler/schema.py)

Each exchange speaks its own JSON. Each feedhandler subclass translates that into
two neutral dataclasses whose fields map 1:1 onto the q columns:

```python
class Side(str, Enum):
    BUY = "B"       # buyer-initiated: aggressor lifted the ask
    SELL = "S"      # seller-initiated: aggressor hit the bid
    UNKNOWN = "U"

@dataclass(slots=True)
class Trade:
    event_time_ns: int   # exchange time, unix nanoseconds
    recv_time_ns: int    # receive time, unix nanoseconds
    symbol: str
    exchange: str
    price: float
    size: float
    side: Side

@dataclass(slots=True)
class Quote:
    event_time_ns: int
    recv_time_ns: int
    symbol: str
    exchange: str
    bid: float
    ask: float
    bsize: float
    asize: float
```

`slots=True` avoids per-instance dicts (these are created at high rate).

### The C++ view types (cpp/include/execution/types.hpp)

On the C++ side the same concepts appear as plain structs the whole terminal
shares: `Quote`, `Signal` (a trade intention), `Order`, `Fill`, and `Position`.
`Position` carries the PnL math (see Section 8). These have no dependencies so the
OMS, risk manager, matching engine, and GUI all speak in terms of them.

`AssetClass` (`Crypto` or `Stock`) was added when stocks became a second
data source (Section 5.7); `Quote` carries one so a row's origin is known
even before the OMS/risk/matching code -- which stays fully asset-agnostic --
ever looks at it. `Quote::touch(side)` was added alongside it: a deep,
continuously-quoted book (Coinbase) always has both a bid and an ask, but a
single-venue feed like Alpaca/IEX can legitimately show a real bid with a
momentarily empty ask. Before `touch()` existed, three separate call sites
(notional-to-quantity sizing in the engine, the OMS's buying-power
reference price, and the paper matching engine's fill price) each computed
`side == Buy ? ask : bid` with no fallback, so a one-sided IEX quote would
silently block both order sizing and fills for that symbol -- crypto never
exercised this path since Coinbase's book is essentially always two-sided.
`touch()` centralizes the fix: try the correct side, fall back to the
other side's real quote, and only fall back to a synthetic mid if there is
truly no data at all.

---

## 5. Component deep-dive

### 5.1 Feedhandler (Python)

The feedhandler is a small, source-agnostic framework plus one class per venue.

**BaseFeedHandler (base.py)** owns everything venue-independent:

- **Buffering.** Subclasses call `emit_trade(Trade)` / `emit_quote(Quote)`, which
  append to in-memory lists. If a list reaches `max_buffer` (default 1000), it is
  flushed immediately.
- **The flush loop.** An asyncio task wakes every `flush_interval_s` (default
  0.25 s), swaps the buffers out, and hands them to the publisher. Flushing on a
  timer amortizes IPC cost -- one call with N rows beats N calls. Crucially, the
  flush runs on the **event-loop (main) thread**, because PyKX's embedded q is not
  thread-safe.
- **The stats loop.** Prints throughput every 5 s.
- **The control loop.** Every 2 s it asks the tickerplant for the list of
  terminal-requested symbols and subscribes to any new ones (Section 7).
- **TLS.** `_build_ssl_context()` uses certifi's CA bundle, with an
  `EL_INSECURE_SSL=1` debug escape hatch for TLS-intercepting proxies.
- **Lifecycle.** `run()` gathers the venue's `_run()` plus the flush/stats/control
  loops with `asyncio.gather`, and drains buffers on shutdown.

A concrete venue implements one method, `_run()`, which connects and calls
`emit_trade` / `emit_quote`. Adding a venue is writing one subclass and
registering it in `__main__.py`.

**CoinbaseFeedHandler (coinbase.py)** is the primary venue:

- Connects to `wss://ws-feed.exchange.coinbase.com` and subscribes to the
  `matches` (trades) and `ticker` (top-of-book) channels. Both are public; no
  authentication is needed.
- **Trade parsing.** A `match` message carries `side` = the side of the *maker*
  (resting) order. The aggressor is the opposite, so we invert it: maker `sell`
  means the taker was a buyer, i.e. `Side.BUY`.
- **Quote parsing.** A `ticker` message carries `best_bid` / `best_ask` and sizes,
  which become a `Quote`.
- **Timestamps.** Coinbase sends ISO-8601 UTC strings; `_iso_to_ns` parses them to
  unix nanoseconds at microsecond precision.
- **Catalog.** On startup it fetches `https://api.exchange.coinbase.com/products`
  (REST), keeps the online USD/USDC product ids, and publishes them to the
  tickerplant as `products` (Section 7).
- **Dynamic subscription.** It keeps a reference to the open socket so the control
  loop can send additional `subscribe` messages at runtime.

**BinanceFeedHandler (binance.py)** is the alternate venue (combined `@trade` +
`@bookTicker` streams; the `m` flag gives the aggressor side). Binance is
geo-blocked in some regions (HTTP 451), which is why Coinbase is the default.

**MockFeedHandler (mock.py)** synthesizes a random walk with no network, for
offline development and testing.

**TickerplantPublisher (publisher.py)** is the boundary to q. Given a batch of
dataclasses it builds one typed PyKX column vector per field --
`TimestampVector`, `SymbolVector`, `FloatVector`, `CharVector` -- and calls
`.u.upd[table; columns]` on the tickerplant with `wait=False` (a one-way async
send, so the feedhandler never blocks). Column vectors line up exactly with the q
schema. It also exposes the control-plane helpers `query_requested`,
`set_products`, and `set_universe`.

**universe.py** holds `TOP_CRYPTO`, ~100 popular Coinbase USD products by market
cap. With no `--symbols` argument the Coinbase feed boots this universe.

**__main__.py** is the CLI: `python -m feedhandler --venue coinbase|binance|mock
[--symbols A,B]`. It builds `Config`, picks the venue class, and runs it.

### 5.2 Tickerplant, tp.q

The tickerplant is deliberately dumb: it routes and it holds a little control
state. It does no analytics and stores no ticks of its own.

- `\p 5010` opens its IPC port; `\l kdb/schema.q` loads the table definitions.
- `subscribers:()!()` is a dictionary mapping a table name (a symbol) to a list of
  subscriber socket handles.
- `.u.sub[t]` records the caller's handle (`.z.w`) as a subscriber of table `t`.
  The RDB calls this once for `trade` and once for `quote`.
- `.u.upd[t;data]` is what feedhandlers call. It forwards the update
  **asynchronously** to every subscriber: `{[h;t;data] (neg h)(`.u.upd;t;data)}`.
  The negative handle means fire-and-forget, so a slow subscriber never blocks the
  feed.
- `.z.pc[h]` is the connection-close callback; it drops a dead handle from every
  subscriber list.
- Control-plane state: `requested` (symbols the terminal wants), `products` (the
  catalog), `universe` (what the feed is actually streaming), plus the setters
  `.u.addsym`, `.u.setproducts`, `.u.setuniverse` (Section 7).

### 5.3 Real-time database, rdb.q

The RDB is the read model.

- `\p 5011` opens its query port; it `hopen`s the tickerplant on 5010 and
  subscribes to `trade` and `quote`.
- Its `.u.upd[t;data]` does two things: `t insert data` appends the batch into the
  in-memory table, then it refreshes a **last-value cache**:
  - `lastquote` is a table keyed by `sym` holding the latest bid/ask/bsize/asize.
  - `lasttrade` is keyed by `sym` holding the latest price.
  - After inserting, it takes the just-appended rows (`(neg n)#quote`), reduces
    them to one row per symbol, and upserts into the cache **in place** with the
    ``` `lastquote upsert ... ``` form (upsert-by-name; the plain
    `lastquote upsert ...` would compute a new table and discard it -- a real bug
    that was caught during development).
- Query surface:
  - `snap[syms]` returns an unkeyed table with one row per requested symbol:
    `sym, bid, ask, bsize, asize, price`. It reads from the caches
    (`0!(select from lastquote where sym in syms) lj lasttrade`), so it is
    O(number of requested symbols), not O(rows in the growing tick tables). This
    is what keeps the terminal responsive after millions of ticks accumulate.
  - `hist[sym;n]` returns the last `n` trade prices for a symbol as a float
    vector, used to draw the price chart.
  - `counts[]` reports row counts and mean ingestion latency (a dev helper).

There is currently no end-of-day flush to an on-disk historical database; the RDB
is memory-only for the life of the process. That is a documented next step.

### 5.4 KdbClient (C++ IPC)

`KdbClient` (kdb_client.hpp/.cpp) is the C++ terminal's connection to a q process.
It implements the q IPC protocol over a raw TCP socket (Section 6). Public API:

- `connect(host, port, creds)` -- opens the socket and performs the q login
  handshake; sets `TCP_NODELAY` for low latency.
- `snapshot(symbols)` -- calls `snap[...]` and parses the returned table into a
  `vector<Quote>`. Dashed tickers (BTC-USD) are passed as ``` `$("BTC-USD";...) ```
  (a string-to-symbol cast) because a bare backtick symbol literal would be parsed
  by q as subtraction.
- `history(symbol, n)` -- calls `hist[...]`, returns a `vector<double>`.
- `products()` / `universe()` -- read the catalog / streaming set (symbol vectors).
- `add_symbol(symbol)` -- fires `.u.addsym[...]` asynchronously (control plane).
- `send_async(expr)` -- one-way message.

Internally, `exec_sync` sends a query and reads the raw response payload;
`query_symbols` / `query_floats` parse a single typed vector; and a small `Cursor`
struct walks the response bytes with bounds checks. It is not thread-safe: only
the engine thread ever calls it.

### 5.5 TradingEngine (C++ threaded core)

`TradingEngine` (engine.hpp/.cpp) is the desk. It owns all mutable trading state
and runs it on **one background thread**, so KDB+ client state (which is not
thread-safe) lives on exactly one thread and the GUI never blocks on IPC.

It holds **two** `KdbClient`s and (since stocks were added, Section 5.7) one
`StockFeed`:

- `kdb_` connects to the RDB (5011) for `snap` and `hist`.
- `ctrl_` connects to the tickerplant (5010) for the control plane (`add_symbol`,
  `products`, `universe`).
- `stocks_` polls Alpaca for whatever stock tickers have been added, on its
  own background thread (never the engine thread -- HTTP round-trips are far
  too slow for the 30ms crypto loop below).

Plus a `Portfolio`, a `RiskManager`, a `PaperMatchingEngine`, an `OrderManager`,
the latest quote per symbol (crypto and stock merged into one `quotes_` map --
the OMS/matching engine reading it never needs to know which asset class a
symbol belongs to), the focused symbol for the chart, cached product and
price-history lists, ring buffers of PnL and equity over time, and now two
parallel watch lists: `symbols_` (crypto) and `stock_symbols_` (stock) --
kept separate because `AddSymbol` routes very differently for each (crypto
goes through the tickerplant's control plane; stock just calls
`stocks_.watch()`), and because a symbol's asset class needs to be known
before a `Quote` for it necessarily exists yet (e.g. to route chart-history
fetches, or to render it under the right Market Watch tab).

The GUI talks to the engine through two thread-safe channels:

- **Commands** (`post(Command)`): the GUI enqueues `Buy / Sell / Flatten /
  AddSymbol / SetFocus / Kill` under a mutex. `AddSymbol` (and every
  `SymbolView`/`ClosedPositionView`) now also carries an `AssetClass`.
- **View** (`view()`): the engine republishes an immutable `EngineView` snapshot
  each cycle under a mutex; the GUI copies it out each frame.

The background loop (`run`), ~33 times per second:

1. Drains the command queue and processes each command.
2. Reconnects `kdb_` / `ctrl_` if either dropped.
3. Polls `snap[symbols_]` (crypto) and merges in `stocks_.snapshot()`
   (stock, a cheap mutex copy -- the actual HTTP calls already happened on
   `StockFeed`'s own thread) into the shared quote cache.
4. Periodically (every ~0.5 s) refreshes `hist[focus_]` for a crypto focus,
   or reads `stocks_.history(focus_)` for a stock focus; periodically (~2 s)
   refreshes the catalog and merges the tickerplant's `universe` into the
   crypto watch list (so the terminal auto-populates with whatever the feed
   streams).
5. Samples total PnL and equity into ring buffers.
6. Publishes a fresh `EngineView`.
7. Sleeps 30 ms.

`process(Command)` maps a GUI action to engine work: `AddSymbol` branches on
`AssetClass` -- crypto adds to `symbols_` and tells the tickerplant to stream
it; stock adds to `stock_symbols_` and tells `stocks_` to start polling it --
either way it focuses the chart on the new ticker; `Buy`/`Sell` compute a
quantity from a dollar notional (or use an exact quantity, see Section 5.6)
at `Quote::touch()` (Section 4) and submit an order through the OMS;
`Flatten` closes a position; `Kill` trips the risk kill switch.

### 5.6 The terminal GUI (C++ / ImGui / ImPlot)

`terminal_gui.cpp` is pure presentation. It creates a GLFW window with an OpenGL
3.2 core context, initializes ImGui (docking enabled) and ImPlot, applies a dark
amber Bloomberg-style theme, and runs a render loop.

Each frame:

1. `glfwPollEvents()` then begin new ImGui/ImPlot frame.
2. If the desk has not started, draw a centered "Fund Account" dialog (capital
   input + START DESK). Clicking START constructs and starts the `TradingEngine`.
3. Otherwise copy an `EngineView` snapshot and draw the docked panels:
   - **Account bar** (a menu bar): status (LIVE/DISCONNECTED), cash, equity, PnL,
     and a KILL button.
   - **Market Watch**: tabbed **Crypto** / **Stocks** (added with stocks,
     Section 5.7) -- two differently-sourced, differently-fresh feeds kept
     visually separate rather than mixed into one growing table. Each tab
     has its own "+ Watch" / "+ Add Stock" exact-ticker box (Alpaca's free
     plan has no bulk catalog to browse, unlike Coinbase's) and a live
     bid/ask/mid table; a row click posts `SetFocus`.
   - **Ticker Search**: a filter box over the full crypto product catalog;
     clicking a product posts `AddSymbol`.
   - **Price Chart** (ImPlot): the focused symbol's last-trade prices, amber, with
     ~15% vertical headroom so the line never hugs the window edge.
   - **PnL** (ImPlot): account PnL over time, drawn as two NaN-masked series so it
     is green above zero and red below, with symmetric Y limits (zero centered)
     and a zero reference line. The NaN-mask split only stitches the two
     colored segments together at a sample that lands exactly on zero; a
     fast swing that jumps from positive to negative *between* two samples
     left a visible gap in the line (found via user report, not inspection).
     Fixed by linearly interpolating the fractional x-position where the
     value actually crosses zero and inserting that point into both series,
     so each line now always terminates exactly at the true crossing.
   - **Order Ticket**: the symbol combo mirrors `EngineView.focus` (one
     source of truth -- previously the ticket tracked its own selection
     independently of what was focused elsewhere, so clicking a Market Watch
     row updated the chart but left the ticket pointed at whatever you'd
     picked before; now every focus-changing click anywhere updates the
     ticket too). Shows live bid/ask and, if held, the current position with
     unrealized PnL, plus a `$` **Notional** / **Qty** mode toggle and a
     rough mid-price size preview. BUY / SELL / **SELL ALL** (renamed from
     FLATTEN for plainer language) gray out with a tooltip explaining why
     (no quote yet, no cash, no position to sell) instead of silently
     rejecting and only logging it.
   - **Positions**: tabbed **Current** (open positions, live unrealized PnL,
     click a row to load it into the Order Ticket in Qty mode pre-filled at
     that exact size, per-row flatten) / **Previous** (closed round trips --
     entry, exit, realized PnL). A position moves from Current to Previous
     the instant it nets back to flat; see Section 8's `ClosedPosition` for
     how `Portfolio` detects that transition.
   - **Event Log**: fills, rejects, connection events.
4. The default docked layout is built once with ImGui's DockBuilder (left:
   watch/search; center: price chart over PnL; right: ticket over positions/log).
   The tabs above live *inside* existing panels rather than changing this
   grid -- adding a second asset class this way avoided needing to redesign
   the docked layout itself.
5. Render the ImGui draw data through OpenGL and swap buffers (vsync, ~60 fps).

Numbers are formatted with thousands separators (`commafy`) and PnL values with an
explicit sign (`signed_money`). The GUI never touches trading state directly -- it
reads the snapshot and posts commands.

### 5.7 AlpacaClient + StockFeed (C++ stock market data)

Stocks were added as a second, independently-sourced asset class after the
crypto-only version above was already working. Two small classes, both in
`cpp/include/execution/`:

**AlpacaClient** (`alpaca_client.hpp`/`.cpp`) -- a libcurl-based REST client
against Alpaca's free "Basic" market-data plan. `latest_quote(symbol)` GETs
`/v2/stocks/{symbol}/quotes/latest`; `daily_bars(symbol, n)` GETs
`/v2/stocks/{symbol}/bars`. Two real API gotchas were found and fixed
empirically (not documented clearly by Alpaca, discovered by testing against
the live API and inspecting the actual bytes back):

- **`feed=iex` must be passed explicitly on every request.** Omitting it
  defaults to the consolidated SIP feed, which free/Basic accounts don't
  have access to -- a 403, not an empty response, so it's easy to misread as
  an auth problem rather than a missing query parameter.
- **`start` must be passed explicitly on the bars endpoint.** Omitting it
  defaults to *today*, silently returning a single (possibly incomplete)
  bar instead of a history window -- the price chart requires >= 2 points to
  render, so this manifested as the chart perpetually showing "waiting for
  trades..." with no error anywhere. Fixed by computing an explicit `start`
  date generously far back (`n * 2 + 30` calendar days, to comfortably cover
  `n` trading days past weekends/holidays) via `start_date_iso()`.

**StockFeed** (`stock_feed.hpp`/`.cpp`) -- owns a background thread separate
from both the engine thread and the crypto `KdbClient`s, because an HTTP
round-trip (tens to hundreds of ms) would stall the engine's 30ms loop (and
therefore crypto polling too) if done inline. Refreshes each watched
symbol's quote at most once every 10 seconds, round-robin, keeping steady-
state load at `symbols / 10` requests/second -- comfortably under Alpaca's
200 requests/minute free-tier limit even with dozens of symbols watched.
Refetches daily bars every 5 minutes (they're daily granularity; no need to
hammer this). `watch()`/`snapshot()`/`history()`/`errors()` are all
documented thread-safe, so `TradingEngine` calls them directly from its own
thread without any extra locking on the engine side.

---

## 6. The q IPC protocol, byte by byte

Because the C++ client speaks q IPC directly, here is exactly what goes over the
wire. Everything is little-endian on a same-host connection.

### Login handshake

After the TCP connect, the client sends its credentials, a capability byte, and a
null terminator, then reads one byte back:

```
send:  <credential bytes> 0x03 0x00      (0x03 = IPC capability version 3)
recv:  0x03                              (one byte: the negotiated capability)
```

A one-byte reply means success. The RDB and tickerplant run without
authentication, so any credential string is accepted (the client sends "el").

### Message framing

Every message (in either direction) is an 8-byte header followed by a serialized
payload:

```
byte 0     : endianness      (1 = little-endian)
byte 1     : message type     (0 = async, 1 = sync request, 2 = response)
byte 2     : compressed flag   (0 = uncompressed; we never send compressed and
                                reject compressed responses -- snapshots are small)
byte 3     : 0
bytes 4..7 : total message length in bytes, uint32 (header + payload)
```

### Sending a query

A query string is serialized as a q char vector (type code 10) and sent as a sync
request (message type 1). For the query text `Q` of length `n`:

```
header:  01 01 00 00  <len=8+6+n : uint32>
payload: 0A 00 <n : int32> <Q bytes>
         ^type ^attr ^count  ^the characters
```

The server evaluates the char vector as q code and replies with a response
message (type 2) whose payload is the serialized result.

### Parsing a response

The client parses only the shapes it needs. Type codes and layouts:

- **Error, type -128 (byte 0x80):** followed by a null-terminated error string.
  `snapshot` surfaces this as "q error: ...".
- **Symbol vector, type 11 (0x0B):** `0B <attr> <count:int32> <count null-terminated strings>`.
  Used for `products` and `universe`.
- **Float vector, type 9 (0x09):** `09 <attr> <count:int32> <count doubles (8 bytes each)>`.
  Used for `hist`.
- **General list, type 0 (0x00):** `00 <attr> <count:int32> <count nested values>`.
- **Dictionary, type 99 (0x63):** two values back to back: keys, then values.
- **Table (flip), type 98 (0x62):** an attribute byte, then a dictionary whose
  keys are a symbol vector of column names and whose values are a general list of
  column vectors.

So the `snap` result (a table) deserializes as: `62` (table) `00` (attr) `63`
(dict) then a symbol vector of column names (`sym bid ask bsize asize price`) then
a general list whose first column is a symbol vector and the rest are float
vectors. The client reads columns by name, so column order does not matter.

A real subtlety encoded here: symbol literals. `snap[`BTC-USD]` fails, because q
tokenizes `` `BTC-USD `` as `` `BTC `` minus the variable `USD`. The client builds
`snap[`$("BTC-USD";"ETH-USD")]` -- a list of strings cast to symbols with `` `$ ``
-- so dashed tickers work.

---

## 7. The control plane

The control plane is what makes the terminal able to add any ticker at runtime and
browse a catalog, without restarting anything. It lives on the tickerplant because
the feedhandler already connects there.

Three pieces of state on the tickerplant:

- `products` -- the full catalog of tradable products, published once by the feed.
- `universe` -- the symbols the feed is actually streaming right now.
- `requested` -- symbols the terminal has asked the feed to start.

And the flows:

**Catalog (browse).** On boot, the Coinbase feed fetches the product list over
REST, keeps the online USD/USDC ones, and calls `.u.setproducts`. The engine reads
`products` from the tickerplant every ~2 s and puts it in the view; the GUI's
Ticker Search filters it.

**Boot universe (populate).** With no `--symbols`, the feed subscribes to the
~100-symbol `TOP_CRYPTO` list, intersected with the live catalog (so delisted
entries are dropped -- typically ~95 remain), and publishes that set via
`.u.setuniverse`. The engine merges `universe` into its watch list, so Market
Watch fills out of the box. This keeps a single source of truth (the Python list)
and lets the C++ side auto-discover it.

**Dynamic subscription (add any ticker).** When the user picks a product in Ticker
Search, the GUI posts `AddSymbol`; the engine adds it locally and calls
`.u.addsym` on the tickerplant, appending it to `requested`. The feedhandler's
control loop polls `requested` every 2 s, finds the new symbol, and sends an
additional `subscribe` message on the already-open Coinbase socket. Its ticks then
flow through the tickerplant into the RDB; the engine's next `snap` picks them up
and the terminal charts and trades it. No restart, no reconnect.

---

## 8. Execution logic

All of this is C++, on the engine thread, entirely simulated.

### Order lifecycle (OrderManager, oms.hpp/.cpp)

An order moves through an explicit state machine; illegal transitions are refused:

```
New  ->  Sent  ->  PartiallyFilled  ->  Filled
                \->  Filled
                \->  Cancelled
   \->  Rejected                        (terminal: Filled / Cancelled / Rejected)
```

`submit(Signal, Quote)` builds an `Order`, then runs two gates before anything can
fill:

1. **Business rules (long-only spot).** A BUY requires `cash >= qty * reference
   price`; a SELL requires `net position >= qty` (no shorting). Failing either
   rejects the order with a reason.
2. **Risk limits.** `RiskManager::check` enforces max order quantity, max order
   notional, and max resulting position, and denies everything if the kill switch
   is tripped. Fail-closed.

If both pass, the order transitions to `Sent`, is handed to the matching engine,
and any resulting fills are applied. `flatten(symbol)` is a convenience that market-
sells the whole long position.

### Risk (RiskManager, risk.hpp/.cpp)

`RiskLimits` = { max_order_qty, max_position_qty, max_order_notional }. `check`
returns an allow/deny with a human-readable reason. `kill(reason)` trips a latch
that denies all subsequent checks. This gate is the line between "an execution
layer" and "a script that fires orders", so it runs in front of every order.

### Paper matching (PaperMatchingEngine, matching.hpp/.cpp)

`fill(Order, Quote)` simulates execution against the current top of book: a market
order fills in full at the touch (buy at ask, sell at bid); a marketable limit
order fills at the touch; a non-marketable limit rests (no fill this tick). No
real venue is contacted. This doubles as the backtester and the safety net; a live
router would implement the same `fill` contract later, behind the same risk gate.

### Portfolio and PnL (Portfolio + Position)

`Portfolio` holds `cash`, the per-symbol `Position` map, and the initial capital.

- On a fill, `cash += (buy ? -1 : +1) * qty * price` and the `Position` is updated.
- `Position::apply` uses signed-position accounting: extending a position
  re-averages the cost; reducing or flipping realizes PnL on the closed quantity
  (`(fill - avg) * closed * direction`) and, if flipped, opens the remainder at the
  fill price.
- `Position::unrealized(mark) = (mark - avg_price) * net_qty`.
- Account level: `equity = cash + sum(net_qty * mark)`, and
  `total_pnl = equity - initial_capital = realized + unrealized`, recomputed every
  cycle from the latest marks.

Worked example: fund 10,000. BTC ask 62,000; buy 3,000 notional -> qty
0.048387, cash 7,000, position 0.048387 @ 62,000. Mark rises to 62,500 ->
unrealized = (62,500 - 62,000) * 0.048387 = +24.19, equity 10,024.19. Flatten at
bid 62,500 -> realized +24.19, cash 10,024.19, position flat.

**`ClosedPosition` history (added for the Positions > Previous split,
Section 5.6).** `positions_` (the `map<symbol, Position>`) never removes an
entry once created -- `net_qty` resets to 0 but the map entry, and its
lifetime `realized_pnl`, persist forever, including across multiple
open/close round trips on the same symbol. That means the raw map alone
can't distinguish "currently flat, previously traded" from "never opened,"
which is exactly what a Current/Previous UI split needs to know.
`Portfolio::apply(Fill)` now snapshots `net_qty`/`realized_pnl` immediately
before and after calling `Position::apply`; if the position was nonzero and
is now exactly zero, it pushes a `ClosedPosition{symbol, qty, avg_entry,
exit_price, realized_pnl of just this round trip, timestamp}` onto a
`vector<ClosedPosition>`. This fires uniformly whether the close came from
a FLATTEN/SELL ALL, a per-row flatten, or a manual sell that happens to zero
the position exactly -- no OMS changes were needed, only `Portfolio`.

---

## 9. Concurrency and thread-safety

Three execution contexts, each with a clear rule:

- **The feedhandler's asyncio loop (one thread).** All PyKX calls happen here,
  never on a worker thread, because PyKX's embedded q is not thread-safe. Flushing
  is scheduled on the loop, not on an executor. asyncio's cooperative scheduling
  means the socket reader, flusher, stats, and control tasks interleave without
  locks.
- **The engine thread (one thread).** Owns both `KdbClient`s, the OMS, the
  portfolio, and all caches. All KDB+ IPC and all order processing happen here.
  Nothing else touches this state.
- **The GUI thread (main thread).** Renders at ~60 fps. It only reads immutable
  `EngineView` snapshots and posts `Command`s.

The engine/GUI boundary is two mutex-guarded handoffs: a command queue (GUI writes,
engine drains) and a published view (engine writes, GUI copies). The 60 fps render
loop therefore never blocks on a network round-trip, and the ~33 Hz engine loop
never blocks on rendering. This split -- a single-owner engine thread plus
immutable snapshots -- is what makes the terminal both fast and simple to reason
about.

---

## 10. End-to-end workflows, traced step by step

### Boot sequence

1. `scripts/run_stack.sh coinbase` starts `q tp.q` (listens on 5010), then
   `q rdb.q` (listens on 5011, dials 5010, subscribes to trade+quote), then the
   Python feed.
2. The feed connects to Coinbase, fetches the product catalog over REST, and calls
   `.u.setproducts` on the tickerplant.
3. The feed subscribes to the boot universe (~95 symbols) and calls
   `.u.setuniverse`.
4. Trades and quotes begin flowing: feed -> `.u.upd` -> tickerplant -> RDB, which
   appends them and refreshes its last-value caches.
5. The user runs `./build/terminal`. It connects `kdb_` to 5011 and `ctrl_` to
   5010. Within ~2 s the engine merges `universe` into its watch list; Market Watch
   fills.

### A quote's journey (tick to screen)

1. Coinbase sends a `ticker` JSON message on the WebSocket.
2. `CoinbaseFeedHandler._handle_message` parses `best_bid`/`best_ask`/sizes into a
   `Quote`, stamping `recv_time_ns`.
3. `emit_quote` appends it to the buffer.
4. Up to 250 ms later the flush loop swaps the buffer and the publisher builds
   column vectors and calls `.u.upd[`quote; columns]` on the tickerplant.
5. The tickerplant async-forwards `(`.u.upd;`quote;columns)` to the RDB.
6. The RDB appends to `quote` and upserts `lastquote` for that symbol.
7. Within ~30 ms the engine's `snap[symbols_]` reads `lastquote`, updating its
   quote cache, and `publish()` writes a new `EngineView`.
8. On its next frame the GUI copies the view and draws the new bid/ask in Market
   Watch (and, if this is the focused symbol, the price chart updates from `hist`).

### Placing a buy order

1. In the Order Ticket the user selects BTC-USD, enters 3000, clicks BUY.
2. The GUI posts `Command{Buy, "BTC-USD", amount=3000, use_notional=true}`.
3. The engine's next loop drains it. `process` looks up the latest BTC-USD quote,
   computes `qty = 3000 / ask`, and builds a market `Signal`.
4. `OrderManager::submit` runs the buying-power check (cash >= 3000?), then the
   risk check (size/notional/position limits, kill switch).
5. If allowed, the order goes `Sent`, the paper matcher fills it at the ask, and
   `on_fill` updates the order to `Filled`, applies the fill to the portfolio
   (cash down, position up), and logs it.
6. The next `EngineView` shows the new position, updated cash/equity, and a log
   line; unrealized PnL then tracks the mark every cycle.

### Adding a new ticker (control plane)

1. In Ticker Search the user types "SOL" and clicks `SOL-USD`.
2. The GUI posts `Command{AddSymbol, "SOL-USD"}`.
3. The engine adds SOL-USD to `symbols_`, calls `ctrl_.add_symbol("SOL-USD")`
   (which fires `.u.addsym[`$"SOL-USD"]` on the tickerplant), and focuses the chart
   on it.
4. The tickerplant appends SOL-USD to `requested`.
5. Within 2 s the feedhandler's control loop reads `requested`, sees SOL-USD, and
   sends a `subscribe` for it on the open Coinbase socket; it republishes
   `universe`.
6. SOL-USD ticks now flow into the RDB. The engine's `snap` (which already includes
   SOL-USD) returns its quotes; Market Watch and the price chart populate; the user
   can trade it.

---

## 11. Failure modes and resilience

- **Exchange disconnect.** Each venue's `_run` wraps the socket in a reconnect
  loop with a delay; on reconnect it re-subscribes to the full current set and
  republishes `universe`.
- **KDB+ disconnect.** `KdbClient` marks itself disconnected on any socket error;
  the engine loop reconnects both clients each cycle. The GUI shows
  DISCONNECTED until it recovers.
- **Slow subscriber.** The tickerplant forwards with async sends (`neg h`), so a
  slow consumer cannot stall the feed. Dead handles are dropped on close.
- **TLS interception.** certifi provides the CA bundle; `EL_INSECURE_SSL=1` is a
  documented debug-only bypass.
- **Backpressure.** Today the design relies on generous buffers and async sends; a
  formal drop policy is deferred.
- **No persistence.** The RDB is memory-only; a process restart loses intraday
  history. An on-disk HDB is the planned fix.

---

## 12. Key design decisions and tradeoffs

- **Pure-socket q IPC client, not `c.o` or `libq`.** KX ships no arm64-macOS
  `c.o`, and `libq` takes over the process on load. Implementing the protocol
  (~250 lines, verified against a live q) keeps the C++ side portable and
  dependency-free. Cost: we maintain a small parser; benefit: no SDK, no runtime
  hijack, works on Apple Silicon.
- **Last-value caches in the RDB.** Polling `snap` for ~95 symbols at 33 Hz over
  ever-growing tables would degrade as data accumulates. Keyed `lastquote` /
  `lasttrade` caches make `snap` O(number of symbols): measured at ~0.085 ms for
  95 symbols. Cost: a few lines in `.u.upd`; benefit: flat latency forever.
- **Single-owner engine thread + immutable view.** Rather than sharing locked
  state between the GUI and the network, one thread owns everything and publishes
  snapshots. This sidesteps most concurrency bugs and keeps the render loop
  non-blocking.
- **Python 3.11, not 3.14.** PyKX 4.0 on 3.14 raises numpy deallocator errors and
  can crash if you ever publish from a worker thread. 3.11 is clean. This is a hard
  environment constraint.
- **Immediate-mode GUI.** For a real-time mirror of engine state, ImGui is far
  simpler than a retained/model-view toolkit. The whole UI is a function of the
  latest snapshot.
- **Control plane on the tickerplant.** The feedhandler already connects there, so
  putting `requested`/`products`/`universe` on the tickerplant avoids extra
  connections and keeps a single source of truth.
- **Long-only spot.** Matches Coinbase spot and keeps the accounting simple.
  Shorting/margin is an easy future relaxation.
- **Alpaca over Alpha Vantage for stocks.** The original plan was Alpha
  Vantage (previous-day close only, 25 requests/day free). Compared against
  Alpaca's free "Basic" plan -- real IEX-venue real-time quotes + historical
  daily bars, 200 requests/minute -- and switched before writing any stock
  code. This removed the originally-planned "yesterday = today" UI framing
  entirely: stocks get genuinely live (single-venue) data, not stale data,
  for the same zero cost.
- **`StockFeed` on its own thread, not folded into the engine loop.** An
  HTTP round-trip is one to two orders of magnitude slower than a q IPC
  round-trip on localhost. Polling Alpaca inline on the engine's 30ms loop
  would have stalled crypto polling too every time a stock quote was due for
  a refresh. A second background thread, rate-limited independently,
  isolates that latency completely -- the engine loop only ever does a cheap
  mutex-guarded cache copy for stocks, never an HTTP call.
- **A Makefile bug found by a heap-corruption crash.** The build had no
  header-dependency tracking (`-MMD -MP`), so `make` only recompiled a
  `.cpp` when *it* changed, not when a header it included changed. Editing
  `engine.hpp` to add `AssetClass` to `SymbolView` without a full `make
  clean` linked a stale `.o` (old struct layout) against fresh ones (new
  layout) -- different translation units disagreeing about a struct's size,
  undefined behavior, manifesting as a `malloc`-detected heap corruption on
  the *second* frame of a headless test. Fixed by adding `-MMD -MP` to the
  build and `-include`ing the generated `.d` files, which is the standard
  fix and should have been there from the start; flagged here because it's
  exactly the kind of bug that looks like a logic error in application code
  but is actually a build-system correctness gap.

---

## 13. Performance characteristics

- **Ingestion.** Batched flushes every 250 ms (or on 1000-tick buffers) with
  one-way async IPC; the feed never blocks on the tickerplant.
- **Query.** `snap` for 95 symbols ~0.085 ms (cache-backed). At 33 Hz the engine
  uses well under 1% of a core on IPC.
- **Rendering.** ~60 fps, vsync-limited; the render loop never performs IPC.
- **End-to-end latency.** Dominated by the flush interval (up to 250 ms) plus the
  engine poll (up to 30 ms). Both are tunable; neither is on a blocking path.

---

## 14. File-by-file reference

```
kdb/
  schema.q       trade / quote table definitions (shared contract)
  tp.q           tickerplant: pub/sub router + control-plane state (:5010)
  rdb.q          real-time DB: tick tables, last-value caches, snap / hist (:5011)

feedhandler/
  schema.py      Trade / Quote dataclasses, Side enum
  config.py      env-driven Config (hosts, ports, flush, buffers, TLS)
  base.py        BaseFeedHandler: buffering, flush/stats/control loops, TLS, run()
  publisher.py   TickerplantPublisher: PyKX bridge + control-plane setters
  coinbase.py    CoinbaseFeedHandler: WS + catalog + dynamic subscribe
  binance.py     BinanceFeedHandler: combined WS streams
  mock.py        MockFeedHandler: synthetic feed (no network)
  universe.py    TOP_CRYPTO boot universe (~100 Coinbase USD products)
  __main__.py    CLI entrypoint (--venue coinbase|binance|mock)
  test_*_parsing.py   deterministic wire-format tests (Binance, Coinbase)
  requirements.txt    pykx, numpy, websockets, certifi

cpp/
  include/execution/
    types.hpp          Side/Order/Fill/Signal/Quote/Position/AssetClass (+ PnL math, Quote::touch)
    portfolio.hpp      cash + positions + ClosedPosition history + equity/PnL accounting
    risk.hpp           RiskManager: limits + kill switch
    matching.hpp       PaperMatchingEngine: fills vs top-of-book
    oms.hpp            OrderManager: state machine + gates + routing
    kdb_client.hpp     pure-socket q IPC client (crypto)
    alpaca_client.hpp  libcurl REST client: Alpaca quotes + daily bars (stocks)
    stock_feed.hpp     background poller wrapping AlpacaClient, own thread
    engine.hpp         TradingEngine: threaded desk, EngineView, Command
  src/
    risk.cpp / matching.cpp / oms.cpp / kdb_client.cpp / alpaca_client.cpp /
    stock_feed.cpp / engine.cpp
    terminal_gui.cpp   ImGui/ImPlot terminal (window, panels, charts)
    selftest.cpp       headless: single-threaded live buy / PnL / flatten
    engine_test.cpp    headless: threaded engine + command/view handoff
    alpaca_test.cpp    headless: Alpaca auth + quote/bars parsing smoke test
  Makefile             make (headless) / make gui / make alpaca-test (-MMD -MP
                        header-dependency tracking -- see Section 12)
  third_party/         vendored imgui (docking) + implot + json (fetched by setup)

scripts/run_stack.sh   launch tp + rdb + feedhandler together
docs/                  architecture.md, phase-1-feedhandler.md, decisions/
web-version/           a separate, self-contained project (React + C++
                        backend, multi-user) -- see Section 16
README.md              overview + run process
design.md              this document
```

---

## 15. How to extend the system

- **Add an exchange.** Subclass `BaseFeedHandler`, implement `_run` (and, for
  runtime tickers, `_subscribe_new`), register it in `__main__.py`. Nothing
  downstream changes.
- **Add a signal source.** Have the Python analysis engine write rows into a KDB+
  `signal` table; add a `KdbClient` query in the engine to read it; feed those
  `Signal`s into `OrderManager::submit` for a rules- or model-driven strategy.
- **Add historical charts.** Backfill Coinbase REST candles into a `bar` table and
  extend `hist` (or add an `ohlc` query) so a chart has shape the instant a ticker
  opens; render candles with ImPlot. `AlpacaClient::daily_bars` already fetches
  OHLC for stocks (Section 5.7) but only the close price is plotted today --
  wiring the full OHLC through and switching the Price Chart to a candlestick
  plot for a stock focus is a smaller version of the same task.
- **Persist ticks.** Add an end-of-day flush from the RDB to a partitioned on-disk
  HDB, so history survives restarts and the analysis engine can train on it.
- **Route real orders.** Implement an exchange-router that satisfies the same
  `fill` contract as `PaperMatchingEngine`, placed behind the existing risk gate,
  and start against an exchange testnet.
- **Add a second asset class.** Follow `alpaca_client.hpp`/`stock_feed.hpp`'s
  shape: a small REST (or IPC) client, a background poller with its own
  thread and rate-limit-aware refresh cadence, a thread-safe
  `watch()`/`snapshot()`/`history()` surface, and a merge point in
  `TradingEngine`'s quote cache. `AssetClass` already has room for a third
  value; the OMS/risk/matching code never needs to change since it only ever
  sees a `Quote`.

---

## 16. The web version (a separate, self-contained project)

`web-version/` began as a companion project in this same repository: a
hosted, multi-user React + C++ backend build reusing this desktop terminal's
trading core (`Portfolio`/`RiskManager`/`PaperMatchingEngine`/
`OrderManager`/`KdbClient`/`AlpacaClient`/`StockFeed`) behind a WebSocket
instead of ImGui, with per-visitor isolated paper accounts sharing one
market-data feed. It has since been made **fully self-contained** -- it no
longer reads, builds, or copies anything from outside its own directory
tree, specifically so it can be relocated to its own path or extracted into
its own git repository without carrying this repo along with it.

Concretely, that meant:

- `web-version/backend/include/execution/*.hpp` and
  `web-version/backend/src/*.cpp` are now this repo's `cpp/include/execution/`
  and `cpp/src/*.cpp` files **copied**, not referenced by relative path.
  This is a deliberate tradeoff, not an oversight: a fix made to
  `Quote::touch()` or `Portfolio`'s `ClosedPosition` tracking in one tree
  does not automatically appear in the other anymore. If both trees are
  actively maintained from a shared origin and this starts to hurt, the
  natural next step is extracting the trading core into a small versioned
  package (or git submodule) both depend on explicitly, rather than either
  hand-copying files or reaching across a relative path that assumes a
  particular checkout layout.

That copy-in was the first step toward independence; the second was
removing a dependency outright rather than copying it. `web-version/`
initially also copied in `kdb/` and `feedhandler/` (this repo's KDB+
tickerplant/RDB and Python ingestion) so its Docker Compose stack could
build standalone. On reflection, that pipeline was only ever doing two
things for the web backend -- cache the latest quote per symbol, keep a
short rolling trade-price history -- which don't need a time-series
database at that project's data volume. `web-version/backend` now connects
to Coinbase directly over a WebSocket it opens itself (`CoinbaseFeed`,
replacing `KdbClient` there), and `web-version/kdb/`+`web-version/
feedhandler/` are gone -- its Docker Compose stack is down to two services
(backend, frontend) with no proprietary license to obtain. This desktop
terminal's own KDB+ pipeline is unaffected -- crypto's live feed here still
has the future "push compute to the data" analytics use case (Section 3's
KDB+ discussion, Section 5) in mind, which `web-version` doesn't need.

See `web-version/design.md` for that project's own complete architecture
writeup (written to stand alone, the same way this document doesn't assume
`web-version/` exists, and including the full reasoning for dropping KDB+)
and `web-version/README.md` for setup/run instructions.
