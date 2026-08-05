# Design Doc — Execution Layer Terminal

A top-to-bottom explanation of how this app is built. Written to be read start to
finish by someone who has never seen the code.

Companion docs: `architecture.md` is the terse reference; `designReview.md` is a
critique of the older design; `simplificationPlan.md` is the migration that got
us here. **This one is the explanation.**

---

## Table of contents

1. [What the app does](#1-what-the-app-does)
2. [The 10,000-foot view](#2-the-10000-foot-view)
3. [The two processes](#3-the-two-processes)
4. [Part 1: the feedhandler (Python)](#4-part-1-the-feedhandler-python)
5. [Part 2: the wire between them](#5-part-2-the-wire-between-them)
6. [Part 3: the terminal (C++)](#6-part-3-the-terminal-c)
7. [Threading: who touches what](#7-threading-who-touches-what)
8. [Following one tick all the way through](#8-following-one-tick-all-the-way-through)
9. [Following one order all the way through](#9-following-one-order-all-the-way-through)
10. [The stock path (a useful contrast)](#10-the-stock-path-a-useful-contrast)
11. [Packaging](#11-packaging)
12. [File map](#12-file-map)
13. [Design decisions and why](#13-design-decisions-and-why)
14. [What is deliberately not built](#14-what-is-deliberately-not-built)

---

## 1. What the app does

You open a desktop window, type in a starting balance, and click START DESK. You
get a trading terminal: live crypto prices from Coinbase (and optionally US
stocks from Alpaca), a price chart, an order ticket, positions, and a running
profit-and-loss figure.

You buy and sell against **real live market prices**, but no order ever leaves
your machine. The fills are simulated against the real bid/ask you're seeing.
It's a flight simulator for trading: real weather, fake plane.

Concretely, the app must:

- Stream live prices for ~95 crypto products simultaneously
- Let you search a catalog of ~400 products and add any of them at runtime
- Turn a click into an order, check it against risk limits, fill it, and update
  your cash and positions correctly
- Redraw all of that at 60fps without stuttering

---

## 2. The 10,000-foot view

```
   ┌─────────────────────┐                    ┌──────────────────────┐
   │  Coinbase           │                    │  Alpaca              │
   │  (WebSocket)        │                    │  (REST/HTTPS)        │
   └──────────┬──────────┘                    └───────────┬──────────┘
              │ live tick stream                          │ polled quotes
              ▼                                           │
   ┌─────────────────────┐                                │
   │  FEEDHANDLER        │  Python                        │
   │  · connect, parse   │                                │
   │  · normalize        │                                │
   │  · batch every      │                                │
   │    250ms            │                                │
   └──────────┬──────────┘                                │
              │ JSON lines over 127.0.0.1:5020            │
              ▼                                           ▼
╔═════════════════════════════════════════════════════════════════════╗
║  TERMINAL (one C++ binary)                                          ║
║                                                                     ║
║   ┌──────────────┐                        ┌──────────────┐          ║
║   │ FeedServer   │                        │ StockFeed    │          ║
║   │ decode JSON  │                        │ poll Alpaca  │          ║
║   └──────┬───────┘                        └──────┬───────┘          ║
║          ▼                                       │                  ║
║   ┌──────────────┐                               │                  ║
║   │ MarketStore  │  last price per symbol        │                  ║
║   │              │  + recent history             │                  ║
║   └──────┬───────┘                               │                  ║
║          └───────────────┬───────────────────────┘                  ║
║                          ▼                                          ║
║              ┌───────────────────────┐                              ║
║              │  TradingEngine        │  the desk                    ║
║              │  ┌─────────────────┐  │                              ║
║              │  │ OMS             │  │  order lifecycle             ║
║              │  │ RiskManager     │  │  limits + kill switch        ║
║              │  │ MatchingEngine  │  │  simulated fills             ║
║              │  │ Portfolio       │  │  cash, positions, PnL        ║
║              │  └─────────────────┘  │                              ║
║              └───────────┬───────────┘                              ║
║                          │ EngineView snapshot                      ║
║                          ▼                                          ║
║              ┌───────────────────────┐                              ║
║              │  GUI (ImGui/ImPlot)   │  draws, never computes       ║
║              └───────────────────────┘                              ║
╚═════════════════════════════════════════════════════════════════════╝
```

**The single most important idea:** data flows *down* this diagram, and commands
flow *up*. The GUI never reaches into trading state; it reads a snapshot and
posts messages. Everything below explains a piece of this picture.

---

## 3. The two processes

| | Feedhandler | Terminal |
|---|---|---|
| Language | Python | C++20 |
| Job | talk to exchanges, normalize ticks | store data, trade, draw |
| Why this language | WebSocket + JSON + reconnect logic is quick to write and easy to change | needs to be fast, and is the thing users install |
| Lifetime | restartable at any time | restartable at any time |

**Why split at all?** Exchange protocol work is fiddly, I/O-bound, and changes
whenever an exchange changes. Python is good at that and its slowness doesn't
matter, because a tick only has to arrive within milliseconds. The trading and
drawing side benefits from being one native binary with no runtime.

**Why not more than two?** Because nothing else needed its own process. (An
earlier version of this project had four. See §13.)

---

## 4. Part 1: the feedhandler (Python)

Directory: `feedhandler/`

### 4.1 The shape

There's one abstract base class and three concrete venues:

```
BaseFeedHandler            (base.py)      — buffering, flushing, control plane
  ├── CoinbaseFeedHandler  (coinbase.py)  — the live one
  ├── BinanceFeedHandler   (binance.py)   — a second real venue
  └── MockFeedHandler      (mock.py)      — synthetic prices, no network
```

A subclass implements exactly one method, `_run()`, which reads from its exchange
forever and calls `emit_trade()` / `emit_quote()`. Everything else — batching,
timing, reconnects, the control plane — lives in the base class.

The mock venue matters more than it looks: it means you can develop and test the
entire system on a plane.

### 4.2 Normalizing

Every exchange has its own message format. `schema.py` defines the two neutral
types everything downstream sees:

```python
@dataclass(slots=True)
class Trade:
    event_time_ns: int   # when the exchange says it happened
    recv_time_ns: int    # when we received it
    symbol: str          # "BTC-USD"
    exchange: str        # "coinbase"
    price: float
    size: float
    side: Side           # who crossed the spread: B, S, or U

@dataclass(slots=True)
class Quote:             # top-of-book
    event_time_ns: int
    recv_time_ns: int
    symbol: str
    exchange: str
    bid: float; ask: float
    bsize: float; asize: float
```

Two details worth understanding:

**Two timestamps, not one.** `event_time_ns` is the exchange's clock;
`recv_time_ns` is ours. The difference is your ingestion latency, and having both
from day one means you can measure it instead of guessing.

**`side` is the aggressor, not the maker.** Coinbase reports the *resting*
order's side, so `coinbase.py` inverts it. If the resting order was a sell, the
person who crossed the spread was a buyer. This matters for any future order-flow
analysis, and getting it backwards is a classic silent bug.

### 4.3 Batching

Ticks aren't sent one at a time. `emit_*` appends to a buffer, and the buffer is
flushed when either:

- 250ms elapses (`EL_FLUSH_INTERVAL`), or
- 1000 ticks accumulate (`EL_MAX_BUFFER`)

At ~150 ticks/second across 95 products, that's roughly 4 messages per second
instead of 150. Per-message overhead dominates at tick rates, so batching is the
single biggest thing keeping this cheap.

### 4.4 The control plane

The terminal needs to say "start streaming SOL-USD" and the feedhandler needs to
say "here's the catalog of everything available." That runs over the same socket:

- Every 2 seconds the feedhandler asks the terminal `{"m":"poll"}`
- The terminal replies with any symbols you've requested since last time
- The feedhandler sends a `subscribe` message on its **already-open** WebSocket

Nothing restarts. You type a ticker, and a second later it's streaming.

### 4.5 Reconnection

Two independent failure modes, handled separately:

- **Exchange drops us** → `coinbase.py`'s loop catches everything, waits 2s,
  reconnects, and re-subscribes to the full current symbol set.
- **Terminal isn't there** → `publisher.py` drops ticks and retries every 2s.

That second one is normal, not exceptional: the terminal only opens its socket
when you click START DESK, so on a fresh boot the feedhandler *always* starts
disconnected and that's fine.

---

## 5. Part 2: the wire between them

One TCP connection on `127.0.0.1:5020`. Messages are **newline-delimited JSON** —
one JSON object per line.

```jsonc
// feedhandler → terminal
{"m":"quotes","rows":[["BTC-USD",64010.1,64010.9,0.4,1.2,1712345678901234567]]}
{"m":"trades","rows":[["BTC-USD",64010.5,1712345678901234567]]}
{"m":"products","syms":["BTC-USD","ETH-USD", ...]}   // the searchable catalog
{"m":"universe","syms":["BTC-USD", ...]}             // what's actually streaming
{"m":"poll"}                                          // "anything requested?"

// terminal → feedhandler
{"m":"requested","syms":["SOL-USD"]}                  // reply to poll
```

Rows are positional arrays rather than objects to keep the payload small —
`["BTC-USD",64010.1,...]` instead of repeating field names 1000 times per batch.

**Why JSON and not a binary format?** At ~4 messages/second carrying a few
hundred rows each, the encoding cost is irrelevant, and you can debug the whole
feed by pointing `nc` at the port. If profiling ever says otherwise, the codec is
swappable without anything above `feed_server.cpp` knowing.

**Why localhost TCP and not a Unix socket or shared memory?** TCP works
identically on macOS and Linux, survives either side restarting, and lets you run
the feed on a different machine if you ever want to. The performance difference
at this volume is not measurable.

---

## 6. Part 3: the terminal (C++)

Directory: `cpp/`

### 6.1 FeedServer — the front door

`feed_server.hpp/cpp`

Listens on 5020, accepts one feedhandler at a time, and reads lines on its own
thread. For each line it decodes the JSON and calls straight into `MarketStore`.

Three things it does deliberately:

- **Never trusts input.** A malformed line is dropped, not fatal. The parse uses
  `allow_exceptions=false` and every field is bounds-checked.
- **Accepts reconnects forever.** When the feedhandler dies, the loop goes back
  to `accept()`. You never restart the terminal because the feed hiccupped.
- **Marks its sockets close-on-exec.** Otherwise the feedhandler child would
  inherit the *listening* socket and, if the terminal crashed, keep port 5020
  held hostage so the next launch couldn't bind.

### 6.2 MarketStore — the whole database

`market_store.hpp/cpp` — about 150 lines, and it is the entire storage layer.

```cpp
std::map<std::string, Quote> last_quote_;              // newest price per symbol
std::map<std::string, std::deque<double>> history_;    // last ≤512 trade prices
std::vector<std::string> products_, universe_, requested_;
std::mutex mu_;
```

That's it. The terminal only ever asks two questions of its market data:

1. *"What is BTC-USD trading at right now?"* → one map lookup
2. *"What were the last 300 prices, for the chart?"* → one deque slice

**History is capped at 512 per symbol on purpose.** An unbounded tick log would
grow forever, and nothing in the app ever reads more than the last 300 entries.
Bounding it makes running out of memory structurally impossible rather than
something you have to remember to prevent.

**`take_requested()` drains rather than accumulates.** When the feedhandler polls,
it gets pending symbols *and they're cleared*. If it merely read a growing list,
every poll would re-deliver every symbol you'd ever added.

### 6.3 TradingEngine — the desk

`engine.hpp`, `engine.cpp`

One background thread running a loop every 30ms:

```
1. drain the command queue        (things the GUI asked for)
2. copy the latest quotes         (from MarketStore + StockFeed)
3. refresh chart history          (every 450ms)
4. refresh the product catalog    (every 2s)
5. publish an EngineView snapshot (what the GUI will draw)
   sleep 30ms
```

The engine owns all mutable trading state and is the **only** thread allowed to
touch it. It does no network I/O whatsoever — both market-data sources are local,
thread-safe caches maintained by other threads.

### 6.4 The execution core

Four small classes, none of which know anything about networks, GUIs, or storage.
This is the part most worth reading if you want to understand trading systems.

**`Portfolio`** (`portfolio.hpp`) — cash, positions, PnL.

Position accounting uses standard signed-average-cost logic: buying more of
something you hold re-averages your cost basis; selling realizes profit on the
portion closed. It also detects when a position returns to exactly zero and
records a *closed round trip*, which is what fills the "Previous" positions tab.

**`RiskManager`** (`risk.hpp`, `risk.cpp`) — the gate that says no.

Checks, in order: kill switch → positive quantity → max order size → max order
notional → max resulting position. **Fail-closed**: anything unexpected is a
rejection, never a pass. Once the kill switch is tripped, every subsequent order
is denied unconditionally — there is no "unkill."

**`PaperMatchingEngine`** (`matching.hpp`, `matching.cpp`) — simulated fills.

A market order fills at the current touch price (buy pays the ask, sell hits the
bid). A limit order fills only if it's already marketable; otherwise nothing
happens. It doesn't model resting in a queue, partial fills from limited depth,
or slippage — see §14.

**`OrderManager`** (`oms.hpp`, `oms.cpp`) — the order lifecycle.

Every order moves through a state machine with enforced legal transitions:

```
New ──► Sent ──┬──► PartiallyFilled ──┬──► Filled      (terminal)
               │                      └──► Cancelled   (terminal)
               ├──► Filled
               ├──► Cancelled
               └──► Rejected                            (terminal)
```

An illegal transition is refused and logged rather than silently applied. Every
submitted order passes **two independent gates** before reaching the matching
engine:

1. **Business rules** — enough cash to buy? enough position to sell? (no shorting)
2. **Risk limits** — the `RiskManager` checks above

Two gates rather than one because they answer different questions. "Can this
account afford it" and "is this trade within policy" fail for different reasons
and you want to know which.

### 6.5 The GUI

`terminal_gui.cpp` — Dear ImGui + ImPlot on GLFW/OpenGL.

Immediate-mode: every frame redraws the whole UI from scratch from an
`EngineView`. There is no retained widget state to get out of sync with reality,
which for a trading display is exactly the property you want.

The GUI thread does two things and only two things:

- reads an `EngineView` snapshot (a mutex-guarded copy, once per frame)
- posts `Command`s (`Buy`, `Sell`, `Flatten`, `AddSymbol`, `Kill`, `SetFocus`)

Panels: account bar with kill switch, Market Watch (Crypto/Stocks tabs), Ticker
Search, Price Chart, PnL chart, Order Ticket, Positions (Current/Previous), Event
Log.

One nice detail: there's a single notion of "the symbol you're looking at"
(`EngineView::focus`). Clicking a row anywhere posts `SetFocus`, and the chart
and order ticket both follow it. No second selection state to disagree.

---

## 7. Threading: who touches what

Four threads. The rules are simple enough to hold in your head:

| Thread | Owns | Reads | Never does |
|---|---|---|---|
| **Engine** | Portfolio, OMS, quote cache | MarketStore, StockFeed | network I/O |
| **FeedServer** | the client socket | — | trading state |
| **StockFeed** | HTTP client, its cache | — | trading state |
| **GUI** | window, widgets | EngineView snapshot | trading state |

Synchronization, in full:

- `cmd_mu_` — the GUI→engine command queue
- `view_mu_` — the engine→GUI snapshot
- `MarketStore::mu_` — guards its maps
- `StockFeed`'s mutexes — guard its caches

**No lock is ever held across I/O.** The engine copies data out under a lock and
then works on the copy. The GUI copies the whole `EngineView` under a lock and
then draws from its copy. Locks are held for microseconds.

**Why publish a whole snapshot instead of sharing state?** Because a trading
display that renders a half-updated portfolio — new cash, old position — shows
you a number that was never true. Copying the entire view atomically means every
frame is internally consistent. The copy costs far less than the bugs it prevents.

---

## 8. Following one tick all the way through

Someone buys Bitcoin on Coinbase. Here's every step to it appearing on screen.

1. **Coinbase** sends a `ticker` message on the WebSocket.
2. **`coinbase.py::_handle_message`** parses the JSON, converts the ISO timestamp
   to unix nanoseconds, builds a `Quote`, calls `emit_quote()`.
3. **`base.py`** appends it to the quote buffer.
4. **Up to 250ms later**, `_flush_loop` fires and hands the batch to the publisher.
5. **`publisher.py`** turns the batch into one JSON line and writes it to the socket.
6. **`feed_server.cpp`** reads the line, splits on `\n`, decodes it, calls
   `MarketStore::on_quote()`.
7. **`market_store.cpp`** takes the mutex and overwrites `last_quote_["BTC-USD"]`.
8. **Within 30ms**, the engine loop copies it into its quote cache.
9. **Same iteration**, `publish()` builds an `EngineView` with the new bid/ask and
   any resulting PnL change.
10. **Next frame**, the GUI copies that view and draws the new number.

End to end: usually under 300ms, dominated entirely by step 4's batching window.
That's a deliberate trade — it's imperceptible to a human, and it cuts message
volume by ~40x.

---

## 9. Following one order all the way through

You type $2,500 and click BUY.

1. **GUI** posts `Command{Buy, "BTC-USD", 2500.0, use_notional=true}` and returns
   to drawing immediately. It does not wait.
2. **Engine thread**, next iteration, picks it off the queue in `process()`.
3. **Looks up the current quote.** No quote → log "no quote yet" and stop.
4. **Converts dollars to quantity**: `2500 / ask`. Notional sizing is the default
   because "buy $2,500 of it" is how people actually think.
5. **Builds a `Signal`** and calls `oms_.submit()`.
6. **Gate 1 — business rules.** Enough cash? If selling, enough position?
   (Shorting is not allowed.) Fails here → `Rejected` with a reason, and the
   order is still recorded so you can see *why*.
7. **Gate 2 — risk.** Kill switch, max order qty, max notional, max position.
8. **Transitions `New → Sent`**, stores the order.
9. **`PaperMatchingEngine::fill`** fills it at the ask.
10. **`on_fill`** updates the average fill price, transitions to `Filled`, and
    applies the fill to the `Portfolio` — cash down, position up.
11. **`publish()`** puts the new cash, position, and PnL into the next `EngineView`.
12. **GUI** draws it, and the Event Log shows `BUY 0.039 BTC-USD @ 64428.81 → FILLED`.

Steps 2–11 all happen on one thread with no locking, which is why this is easy to
reason about: an order is processed start to finish with nothing else mutating
state underneath it.

---

## 10. The stock path (a useful contrast)

Stocks come in completely differently, and comparing the two is instructive.

```
Alpaca REST  →  AlpacaClient (libcurl)  →  StockFeed (own thread)  →  engine
```

`AlpacaClient` (`alpaca_client.cpp`) is a small libcurl wrapper. `StockFeed` runs
a thread that round-robins your watched symbols, refreshing quotes at most every
10s and daily bars every 5 minutes, because HTTP round-trips are far too slow for
a 30ms loop and Alpaca rate-limits.

Note what's the same and what isn't:

- **Different**: pull vs. push, HTTP vs. WebSocket, C++ vs. Python, 10 seconds vs.
  milliseconds
- **Identical**: both produce a `Quote`, land in the engine's cache the same way,
  and go through the same OMS, risk manager, matching engine, and portfolio

The execution core genuinely doesn't know which asset class it's trading. That's
what makes adding a third source cheap.

Stocks are off unless `ALPACA_API_KEY_ID` and `ALPACA_API_SECRET_KEY` are set;
the tab explains this rather than silently showing nothing.

---

## 11. Packaging

`scripts/make_dmg.sh` produces a macOS `.app` inside a `.dmg`:

```
Execution Layer Terminal.app/
  Contents/
    MacOS/
      Execution Layer Terminal   ← shell launcher
      terminal                   ← the C++ binary
    Frameworks/
      libglfw.3.dylib            ← copied from Homebrew, relinked to @rpath
    Resources/
      icon.icns
      feedhandler/               ← PyInstaller bundle: feed + Python + deps
```

The launcher exists because Finder starts apps with `cwd=/` and no shell
environment. It:

1. `cd`s to `~/Library/Application Support/…` so ImGui can save your window layout
2. sources `~/.execution-layer.env` so Alpaca keys can reach the app
3. sets `EL_FEEDHANDLER` to the bundled feed and execs the real binary

`FeedProcess` (`feed_process.cpp`) reads that variable when you click START DESK
and spawns the feedhandler. In a source checkout the variable is unset, so it
does nothing and you run `scripts/run_stack.sh` yourself. Same binary, both
workflows.

The child is spawned with `--exit-when-orphaned`, which makes it poll `getppid()`
and quit if the terminal dies without cleaning up. Without that, force-quitting
the app would leave a Python process streaming Coinbase indefinitely.

Result: ~10MB download, ~26MB installed, and nothing to install first — no
Homebrew, no Python, no database.

---

## 12. File map

```
feedhandler/                Python — talks to exchanges
  schema.py                 Trade / Quote dataclasses (the neutral format)
  base.py                   buffering, flush loop, control plane, TLS
  coinbase.py               Coinbase WebSocket + catalog + live subscribe
  binance.py                Binance WebSocket
  mock.py                   synthetic feed, no network
  publisher.py              batches → JSON lines → the terminal's socket
  universe.py               ~100 default products to boot with
  config.py                 env-var configuration
  __main__.py               CLI entry point

cpp/include/execution/      C++ headers (interfaces + docs)
  types.hpp                 Quote, Order, Fill, Position, Signal, enums
  portfolio.hpp             cash / positions / PnL accounting
  risk.hpp                  limits + kill switch
  oms.hpp                   order lifecycle
  matching.hpp              paper fills
  market_store.hpp          the in-process store
  feed_server.hpp           the localhost socket
  feed_process.hpp          spawns the bundled feed (packaged builds)
  alpaca_client.hpp         Alpaca REST
  stock_feed.hpp            background stock poller
  engine.hpp                TradingEngine, EngineView, Command

cpp/src/                    implementations
  terminal_gui.cpp          the entire GUI
  selftest.cpp              offline assertions over the execution core
  engine_test.cpp           threaded engine against a live feed

scripts/
  run_stack.sh              start the feedhandler (dev)
  make_dmg.sh               build the .app + .dmg
  make_icon.py              generate the icon (stdlib only)
  feedhandler_entry.py      PyInstaller entry point
```

Reading order if you're new: `types.hpp` → `portfolio.hpp` → `oms.cpp` →
`engine.cpp` → `terminal_gui.cpp`.

---

## 13. Design decisions and why

**Storage is a hash map, not a database.**
The app asks its market data exactly two questions (§6.2). An earlier version
routed this through a KDB+ tickerplant and real-time database — two extra
processes, a commercial license, and a hand-written IPC client — to answer those
same two questions. The store now does it in-process in ~150 lines. This is
documented at length in `designReview.md`, including why the older design looked
reasonable at the time.

**Python for I/O, C++ for everything else.**
Exchange plumbing changes often and isn't performance-critical. Trading logic and
rendering are performance-sensitive and ship to users. Each language does what
it's good at, and there's exactly one boundary between them.

**One thread owns trading state.**
Concurrency bugs in an order manager are hard to find and expensive to have. One
owner thread and an immutable published snapshot eliminates the entire category.

**The GUI computes nothing.**
Everything it displays is precomputed in `EngineView`. If a number looks wrong,
there's exactly one place to look.

**Fail-closed risk.**
Every gate defaults to rejection. A bug in risk logic should stop trading, not
allow trading.

**Bounded buffers everywhere.**
Price history caps at 512/symbol, the event log at 200 lines, PnL history at 1800
points. Nothing grows without limit, so nothing needs periodic cleanup.

**The mock venue is a first-class citizen.**
`--venue mock` runs the whole system with no network. Fast tests, offline
development, and demos that can't be broken by an exchange outage.

---

## 14. What is deliberately not built

Being explicit about this so nothing here is mistaken for a finished product.

**No durable history.** The store is memory-only and bounded. Close the app and
today's ticks are gone. The intended fix is a Parquet archive queried with
DuckDB — embedded, no server, no license — but it isn't written.

**No analysis or signal layer.** `types.hpp` defines a `Signal` type the OMS can
act on, and nothing produces one. All orders come from the GUI. Rolling
statistics, models, and automated strategies are a design intention, not code.

**No live order routing.** Everything is paper. There is no venue integration for
sending real orders, and adding one would need authentication, an execution
report path, and reconciliation that don't exist.

**The matching engine is simplistic.** Fills happen instantly and completely at
the touch price. Real execution involves queue position, partial fills against
limited depth, slippage on large orders, and rejections. Small orders on liquid
pairs behave plausibly; large ones would not.

**No backpressure policy.** If the terminal stalls, the socket buffer fills and
the feedhandler eventually sees a dead connection. Reasonable, but not a
considered drop policy.

**Top-of-book only.** Best bid and ask, no order book depth. Coinbase's `level2`
channel requires authentication and is out of scope.

**One feedhandler at a time.** `FeedServer` serves a single publisher. Multiple
simultaneous venues into one terminal would need multiplexing.

**Crypto and stocks aren't unified.** They're separate tabs from separate
sources with very different refresh rates (milliseconds vs. 10 seconds). The
execution core treats them identically; the data paths do not.
