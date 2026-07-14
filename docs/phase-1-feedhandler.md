# Phase 1 — Market-data feedhandler → KDB+

This phase proves the end-to-end loop: real (or simulated) crypto ticks land in
an in-memory KDB+ store and are immediately queryable. Everything else in the
project builds on this backbone.

## What was built

| File | Purpose |
|---|---|
| `kdb/schema.q` | `trade` / `quote` table definitions — the shared contract |
| `kdb/tp.q` | Tickerplant: pub/sub router on port 5010 |
| `kdb/rdb.q` | Real-time DB on port 5011; subscribes to the tickerplant |
| `feedhandler/schema.py` | Normalized `Trade` / `Quote` dataclasses + `Side` enum |
| `feedhandler/config.py` | Env-driven config (symbols, ports, flush, TLS) |
| `feedhandler/publisher.py` | PyKX bridge: batches → typed vectors → `.u.upd` |
| `feedhandler/base.py` | Buffering, flush loop, lifecycle (abstract base) |
| `feedhandler/binance.py` | Binance WebSocket implementation |
| `feedhandler/mock.py` | Synthetic feed (no network) |
| `feedhandler/__main__.py` | CLI: `python -m feedhandler --venue …` |
| `feedhandler/test_binance_parsing.py` | Deterministic wire-format tests |
| `scripts/run_stack.sh` | Launch tp + rdb + feedhandler together |

## How it works

### The feedhandler abstraction

`BaseFeedHandler` owns everything venue-independent:
- **buffering** (`emit_trade` / `emit_quote` append to in-memory lists),
- the **flush loop** (an asyncio task that publishes every `flush_interval_s`),
- a **stats loop** (prints throughput every 5 s),
- **lifecycle** (`run()` gathers the tasks; final drain on shutdown).

A concrete venue subclass implements exactly one method, `_run()`, which reads
its exchange and calls `emit_trade` / `emit_quote` with normalized ticks. Adding
Coinbase later is: write `CoinbaseFeedHandler(BaseFeedHandler)`, register it in
`__main__.py._VENUES`. Nothing else changes. `MockFeedHandler` and
`BinanceFeedHandler` are both just siblings under this contract.

### Publishing to KDB+

`publisher.py` converts a batch of dataclasses into typed PyKX column vectors
(`TimestampVector`, `SymbolVector`, `FloatVector`, `CharVector`) that line up 1:1
with `kdb/schema.q`, then calls `.u.upd[table; data]` on the tickerplant. Column
vectors (not row-by-row inserts) because KDB+ is columnar and IPC has per-message
overhead — one call with N rows beats N calls. Sends are one-way (`wait=False`),
so the feedhandler never blocks on the tickerplant.

### The q side

- `tp.q` keeps a `subscribers` dict (table → handles). `.u.upd` async-forwards each
  update to subscribers; `.u.sub` registers them; `.z.pc` drops dead handles.
- `rdb.q` subscribes to `trade` and `quote`, and its `.u.upd` simply appends the
  incoming column vectors into the local tables. A `counts[]` helper reports row
  counts and mean ingestion latency.

## Verification performed

1. **Schema loads** and both tables have the right column types
   (`meta trade` / `meta quote`).
2. **Mock pipeline end-to-end**: ran `tp.q` + `rdb.q` + the mock feed for ~5 s →
   RDB held **200 trades + 200 quotes** with correct 2026 nanosecond timestamps;
   a pushed-down VWAP query returned sensible per-symbol results.
3. **Binance parsing**: `test_binance_parsing.py` passes — trade price/size, the
   buyer/seller **aggressor** logic (the `m` flag), event-time ms→ns conversion,
   and `bookTicker → quote` mapping.
4. **Binance live connect**: connects and reconnects cleanly; this location is
   geo-blocked (HTTP 451), demonstrating the jurisdiction caveat first-hand.

## Bugs found & fixed during the build

These are worth recording because they'll bite anyone extending the project:

1. **Solitary `/` lines start a q block comment.** The original doc comment blocks
   used blank `/` separator lines. In q, a line that is *just* `/` opens a
   multi-line comment that runs until a solitary `\` — which silently swallowed
   the table definitions, so no tables were created. Fixed by never using a
   solitary `/` (use `/ .` or text).
2. **PyKX embedded q is not thread-safe.** The first design flushed via
   `loop.run_in_executor` (a worker thread), which SIGSEGV'd under load. Fixed by
   flushing on the main/event-loop thread; combined with `wait=False` sends the
   blocking cost is negligible.
3. **Python 3.14 + PyKX 4.0.** Triggers `decref_numpy_allocated_data` deallocator
   errors (numpy buffer-aliasing). Cosmetic at GC but a real hazard. Fixed by
   standardizing on **Python 3.11**.
4. **TLS behind an intercepting proxy.** `CERTIFICATE_VERIFY_FAILED`. Fixed by
   using `certifi`'s CA bundle explicitly, with a debug-only `EL_INSECURE_SSL=1`
   escape hatch (never for order routing).

## How to run

See the [README Quickstart](../README.md#quickstart). Shortest path:

```bash
export QHOME="$HOME/.kx/q" QLIC="$HOME/.kx"
scripts/run_stack.sh mock BTCUSDT,ETHUSDT
# in another shell:
q -c "1000 1000" 2>/dev/null <<< 'h:hopen`::5011; h"counts[]"'
```

## Next (Phase 2)

Port the Stock-Analysis-Engine metrics to run on rolling windows pulled from the
RDB, and publish results into a KDB+ `signal` table for the execution layer to
consume. Split cheap/continuous (in q) from expensive/periodic (in Python) per
[`architecture.md` §4](architecture.md).
```
