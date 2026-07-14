# ADR 0001 -- Crypto exchange choice for the market-data feed

**Status:** Accepted (Phase 1)
**Date:** 2026-07

## Context

We need a real-time source of time-series market data to develop the pipeline.
Crypto exchanges are ideal for *development*: they expose free, public,
unlimited, tick-level WebSocket feeds that trade 24/7 (no market-hours wait, no
market-data licensing). Equities feeds (Alpaca, Polygon, Databento) come later;
because the feedhandler is source-agnostic, switching is a one-class change.

## Binance vs Coinbase (decision inputs)

| | Binance (`.com`) | Coinbase |
|---|---|---|
| Public market-data WS | `wss://stream.binance.com:9443` | `wss://ws-feed.exchange.coinbase.com` |
| Auth for market data | **None** (simplest) | Mixed; `level2` / Advanced-Trade need JWT |
| Trade stream -> `trade` | `<sym>@trade` | `matches` / `market_trades` |
| Quote stream -> `quote` | `@bookTicker` (best bid/ask) | `ticker` |
| Depth | L2 diff (`@depth`) | L2 + **L3 order-by-order** (`full`) |
| Liquidity / tick rate | Highest globally | Lower, still ample for dev |
| Jurisdiction | **Blocks restricted locations (e.g. US)** | US-regulated, available |

## Decision

**Start with Binance** for the first feedhandler because market data needs no
auth (trivial to plumb) and the tick rate is ideal for stress-testing. Keep the
feedhandler abstracted so **Coinbase** can be added as a sibling class, and so
either can be the live source without downstream changes.

## Consequences / observed

- The abstraction (`BaseFeedHandler`) is validated: `MockFeedHandler` and
  `BinanceFeedHandler` are siblings; Coinbase slots in the same way.
- **The jurisdiction caveat is real and was hit during Phase 1 testing:**
  Binance.com returned **HTTP 451 "Service unavailable from a restricted
  location"** from this dev machine. If you are in a restricted location, use
  **Coinbase** (next feedhandler to implement) or **Binance.US**, or run from an
  eligible location.
- Because of this, Coinbase is likely the first *live* venue for many users even
  though Binance was implemented first. That's fine -- same interface.

## Follow-ups

- [x] **`CoinbaseFeedHandler` implemented** (`feedhandler/coinbase.py`) using the
  public `ticker` + `matches` channels -- no auth. Verified live end-to-end
  (real BTC-USD/ETH-USD ticks into the RDB). It is now the recommended live venue
  for restricted locations. Add JWT auth for `level2`/`full` if L3 depth is wanted.
- Consider a `--venue all` mode that runs multiple feedhandlers into the same
  tickerplant, tagged by the `exch` column.
```
