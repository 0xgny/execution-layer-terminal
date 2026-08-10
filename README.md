# Execution Layer Terminal

This is a real-time paper-trading terminal that spans two asset classes. Crypto
streams live from Coinbase; stocks come from Alpaca's free market-data API (real
IEX-venue quotes plus historical daily bars). A C++ terminal consumes both so I
can trade a simulated account against real prices, with live PnL, price and PnL
charts, and an order blotter.

It grew out of q-sim, an earlier KDB+/PyKX tick pipeline of mine. The streaming
design carried over; the KDB+ dependency did not. I also plan a research/signal
layer, but I want to be clear that **it is not built yet** — see `architecture.md`
Sec. 4.

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
in-process last-value cache plus a bounded per-symbol price history, and the
Python feedhandler publishes batched ticks into it over a localhost socket.
Python does ingestion; C++ does storage, execution, and the UI.

Stocks are a second, independent source. The terminal talks to Alpaca directly
over HTTPS on its own thread, so a slow HTTP round-trip never stalls the crypto
path or the GUI. Swapping either data source is isolated to one class — a
`BaseFeedHandler` subclass for crypto venues, `AlpacaClient`/`StockFeed` for
stocks.

For the full system design — component map, data flow, the feed wire format, the
threading model, and what is built versus planned — see `architecture.md`.

---

## Why I started with KDB+, and why I moved off it

### Why I chose KDB+ first

I carried the design over from q-sim, my earlier tick pipeline, and I picked it
deliberately rather than by inertia:

- **It's the industry standard for tick data.** Columnar, in-memory, built for
  time-series queries. If I'm building a trading system, that's the tool the
  field actually uses.
- **The tickerplant/RDB split is the canonical shape.** A tickerplant routes and
  writes a replay log, a real-time database stores the intraday tape and answers
  queries, clients subscribe. I wanted to build that properly, not approximate it.
- **One IPC fabric for two languages.** Python for ingestion and C++ for
  execution could share the same data without me writing a bespoke bus.
- **It gave me somewhere to put analytics.** The plan was to push
  volume-reducing aggregations (VWAP, order-flow imbalance, rolling correlation)
  into q on the server rather than shipping millions of rows to Python.

I also wrote the KDB+ client, before removing it. KX doesn't publish its prebuilt C client for
arm64 macOS, so I implemented the q IPC handshake and the deserializers I needed
directly over a socket.

### Why I moved off it

Eventually I measured what the application actually asked of the database, and
the answer was uncomfortable:

- **The workload didn't justify the machinery.** Two queries — a per-symbol
  last-value lookup and the last 300 trade prices of the charted symbol — against
  a `quote` table where nine of fifteen columns were never read. Scanning it was
  too slow for a 30ms poll, so I maintained my own last-value tables in q: a
  columnar analytics engine used as a hash map, with a hash map bolted on to make
  it fast enough. None of the analytics that justified it existed yet.
- **The complexity was the real cost.** Three processes, two languages, a
  hand-rolled wire protocol and a licensed database is a lot to hand someone who
  wants to contribute — you can't run the thing, let alone change it, without a
  kdb+ license and enough q to debug it. It's also a lot for me to keep alive
  over years. Every dependency there was one more thing that could rot.
- **I want this on the web and as a macOS app.** Shipping it that way means
  shipping whatever it depends on, and a commercial kdb+ license is real friction
  in that path — for hosting, for distribution, for anyone who just wants to
  download it and run it. Giving up fractions of a second and using plain old
  C++ instead is a trade I'll take every time.

The clincher was already in my own repo. The stock path — Alpaca → `AlpacaClient`
→ `StockFeed` → the same `Quote`, the same OMS, the same GUI — reached the exact
same place in ~240 lines of C++ with no database, no second language, and no IPC.
The crypto path needed three processes, a licensed database, and a hand-rolled
wire protocol to deliver an identical result. I'd unintentionally run a
controlled experiment and the simple side won.

So I replaced it with `MarketStore`: a `std::map` of last quotes and a bounded
per-symbol price deque, about 150 lines. That deleted two processes, a commercial
license, 183 lines of q, my 266-line IPC client, and the PyKX dependency that was
pinning me to Python 3.11 — with **zero** change in what the app does.

### What I gave up

A real analytical engine over history. My store is memory-only and bounded, so
when I build the analysis layer I'll need durable history — I plan to use Parquet
files queried with DuckDB (embedded, columnar, no server, no license). KDB+ is
genuinely excellent at what it does. It was solving a problem I didn't have yet,
and I was paying for it in every dimension that mattered: setup, distribution,
dependencies, and the number of moving parts I had to keep alive to see a price
on screen.

---

## Running it

Two processes: the feedhandler and the terminal. One script starts both.

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
```

### 2. Alpaca keys (optional — enables the Stocks tab)

Without these the terminal runs crypto-only and the Stocks tab says so.

Everything the app needs lives in the repo. Copy the template and fill it in:

```bash
cp .env.example .env
```

Then uncomment these two lines in `.env`:

```
ALPACA_API_KEY_ID=PK...
ALPACA_API_SECRET_KEY=...
```

`.env` is gitignored, so real keys stay out of git — `git add .env` is refused.
`.env.example` is the committed template and should never hold real keys.

`scripts/run.sh` sources `.env` and reports which mode it's in:

```
[run] loaded /path/to/execution-layer/.env
[run] Alpaca configured -- Stocks tab enabled
```

`.env` also carries the optional feed-port and feedhandler tuning variables; see
the comments in the file.

Free keys from [alpaca.markets](https://alpaca.markets) are enough — the "Basic"
plan gives real IEX-venue quotes and daily bars. No trading permission is needed,
since this app never routes real orders.

The keys are read once, when the desk starts, so I restart the terminal after
editing `.env`. A bad key shows up as an `alpaca HTTP 401` line in the Event Log
rather than failing silently.

### 3. Run

```bash
scripts/run.sh
```

That builds the terminal if needed, starts the Coinbase feedhandler in the
background, opens the window, and shuts the feed down when I close it.

```bash
scripts/run.sh mock                       # synthetic prices, no network
scripts/run.sh coinbase BTC-USD,ETH-USD   # just these two
scripts/run.sh binance BTCUSDT,ETHUSDT    # a different venue
scripts/run.sh --feed-only                # feed alone (for engine_test)
scripts/run.sh --no-feed                  # terminal alone
```

With no symbol list, Coinbase boots the full top-crypto universe (~95 live USD
products), so Market Watch is populated immediately.

### 4. Trade

1. Enter a starting balance and click **START DESK**.
2. Market Watch fills with the live crypto universe. If Alpaca is configured,
   the Stocks tab takes an exact ticker (e.g. `AAPL`) in "+ Add Stock" — there's
   no bulk stock catalog to browse, unlike crypto's Ticker Search, so I type the
   symbol directly.
3. Click any row to chart it.
4. In the Order Ticket, pick a symbol, choose `$` notional or exact `Qty`, and
   BUY / SELL.
5. Watch the price chart and the PnL chart (green in profit, red in loss).
6. Close a position with SELL ALL, or the per-row flatten in Positions >
   Current. It then moves to Positions > Previous with its entry, exit, and
   realized PnL. Clicking a Current position jumps to the Order Ticket
   pre-filled at that exact size, to top up or exit precisely.

Close the window to stop; the script tears the feed down.

Until you click START DESK the terminal isn't listening, so the feedhandler reports
that it's dropping ticks and retrying. That's expected — the two reconnect to
each other automatically, in either order.

### Running headless

```bash
cd cpp
make run-selftest    # offline, no network: asserts the OMS/risk/PnL pipeline
make engine-test     # threaded engine + command/view handoff; needs a feed
```

`selftest` is self-contained and exits non-zero on failure. `engine-test` wants a
feedhandler alongside it — `scripts/run.sh --feed-only` is enough.

---

## Requirements

- macOS on Apple Silicon. **The GUI is macOS-only** as it stands: `cpp/Makefile`
  links `-framework OpenGL/Cocoa/IOKit/CoreVideo` and resolves GLFW via Homebrew.
  The headless targets build anywhere; porting the GUI to Linux would need an
  X11/Wayland GLFW backend and dropping the framework flags. I haven't tested it
  there.
- Python 3.11 — what I develop and test against. Nothing in the feedhandler
  should require 3.11 specifically, but I haven't tested newer versions.
- A C++20 compiler (Apple clang works).
- GLFW, plus the vendored Dear ImGui, ImPlot, and nlohmann/json fetched above.
- libcurl, which ships with macOS, for the stock feed.
- Optional: a free [Alpaca](https://alpaca.markets) account for stocks.

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
                            feed_server, feed_process, alpaca_client,
                            stock_feed, engine
    src/                   implementations + terminal_gui.cpp + headless tests
    Makefile               build (make gui / make all / make alpaca-test)
    README.md              terminal design + build detail
  scripts/run.sh           build and run everything
  .env.example             template for .env (gitignored local config/keys)
  architecture.md          system design, data flow, and rationale
```

---


## Roadmap

Done: live feedhandler (Binance + Coinbase); in-process market store; threaded
engine with paper OMS, risk gate, and matching; docked GUI with account bar,
tabbed market watch (crypto + stocks), ticker search, order ticket, tabbed
positions (current/previous), event log, and live price + PnL charts; runtime
control plane to add any crypto ticker; top-crypto boot universe; stocks via
Alpaca's free real-time IEX data on a dedicated polling thread.

Next: candlestick / OHLC charts with historical backfill; durable tick history
(Parquet + DuckDB) to replace the in-memory-only store; an analysis layer
producing signals for the engine to consume; optional live order routing to an
exchange testnet behind the risk gate; and a distributable macOS build.
