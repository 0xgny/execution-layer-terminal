/ ============================================================================
/ schema.q  --  Table definitions for the Execution Layer tick store
/ ============================================================================
/ These tables are the shared contract between:
/   * the Python feedhandler (writes ticks in)
/   * the RDB (stores them in memory)
/   * the analytics / execution layers (read them out)
/ .

/ Design decisions (and how they differ from the original q-sim prototype):
/ .

/   * `time` is a full `timestamp` (date + nanosecond time), NOT a `timespan`
/     (offset-from-midnight). Crypto trades 24/7 across day boundaries, so we
/     must keep the date. This is the exchange-reported event time.
/ .

/   * `recv` is a second `timestamp`: the wall-clock time our feedhandler
/     RECEIVED the message. `recv - time` is our end-to-end ingestion latency,
/     which is worth measuring from day one.
/ .

/   * `size` / `bsize` / `asize` are `float`, not `long`. Crypto quantities are
/     fractional (e.g. 0.0123 BTC). q-sim used `long` because it modelled
/     whole-share equities.
/ .

/   * `exch` (symbol) tags every row with its source exchange, because the
/     feedhandler is designed to multiplex several venues (Binance now,
/     Coinbase later) into one store. See architecture.md
/ .

/   * `side` (char) on trades records the AGGRESSOR side:
/         `B` = buyer-initiated (aggressor lifted the ask)
/         `S` = seller-initiated (aggressor hit the bid)
/         `U` = unknown
/     This lets us compute order-flow imbalance directly from the trade tape
/     without needing an as-of join back to quotes.
/ ============================================================================

trade:([]
    time:`timestamp$();      / exchange event time (date + ns)
    recv:`timestamp$();      / feedhandler receive time (date + ns)
    sym:`symbol$();          / instrument, e.g. `BTCUSDT
    exch:`symbol$();         / source venue, e.g. `binance
    price:`float$();         / trade price
    size:`float$();          / trade quantity (fractional)
    side:`char$() );         / aggressor: `B`uy / `S`ell / `U`nknown

quote:([]
    time:`timestamp$();      / exchange event time (date + ns)
    recv:`timestamp$();      / feedhandler receive time (date + ns)
    sym:`symbol$();          / instrument
    exch:`symbol$();         / source venue
    bid:`float$();           / best bid price
    ask:`float$();           / best ask price
    bsize:`float$();         / size available at best bid
    asize:`float$() );       / size available at best ask
