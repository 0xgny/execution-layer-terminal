"""Deterministic tests for Binance wire-format -> normalized tick parsing.

These exercise BinanceFeedHandler._handle_message with real sample payloads, so
the venue-specific translation is validated without any network access (useful
where Binance is geo-blocked, e.g. HTTP 451 from restricted locations).

Run:  python -m feedhandler.test_binance_parsing
"""

from __future__ import annotations

import json

from .binance import BinanceFeedHandler
from .config import Config
from .schema import Side


class _StubPublisher:
    """Stands in for the real publisher; never touched unless a flush fires."""


def _make_handler() -> BinanceFeedHandler:
    cfg = Config()
    cfg.symbols = ["BTCUSDT"]
    cfg.max_buffer = 10_000  # ensure no flush during the test
    return BinanceFeedHandler(cfg, _StubPublisher())


def test_trade_buy_and_sell_side() -> None:
    h = _make_handler()

    # m=false -> buyer is the aggressor (lifted the ask) -> BUY
    buy = {"stream": "btcusdt@trade", "data": {
        "e": "trade", "E": 1_700_000_000_000, "s": "BTCUSDT", "t": 1,
        "p": "50000.10", "q": "0.25", "T": 1_700_000_000_000, "m": False}}
    # m=true -> buyer is the maker, seller is the aggressor -> SELL
    sell = {"stream": "btcusdt@trade", "data": {
        "e": "trade", "E": 1_700_000_000_500, "s": "BTCUSDT", "t": 2,
        "p": "49999.90", "q": "1.5", "T": 1_700_000_000_500, "m": True}}

    h._handle_message(json.dumps(buy))
    h._handle_message(json.dumps(sell))

    assert len(h._trades) == 2, h._trades
    t0, t1 = h._trades
    assert t0.side is Side.BUY and t1.side is Side.SELL
    assert t0.price == 50000.10 and t0.size == 0.25
    assert t1.price == 49999.90 and t1.size == 1.5
    assert t0.symbol == "BTCUSDT" and t0.exchange == "binance"
    # event time: 1_700_000_000_000 ms -> ns
    assert t0.event_time_ns == 1_700_000_000_000 * 1_000_000
    print("ok: trade parsing + aggressor side")


def test_bookticker_to_quote() -> None:
    h = _make_handler()
    bt = {"stream": "btcusdt@bookTicker", "data": {
        "u": 400900217, "s": "BTCUSDT",
        "b": "50000.00", "B": "3.1", "a": "50001.00", "A": "4.2"}}

    h._handle_message(json.dumps(bt))

    assert len(h._quotes) == 1, h._quotes
    q = h._quotes[0]
    assert q.bid == 50000.00 and q.ask == 50001.00
    assert q.bsize == 3.1 and q.asize == 4.2
    # bookTicker has no exchange timestamp -> event time stamped on receive
    assert q.event_time_ns == q.recv_time_ns
    print("ok: bookTicker -> quote")


if __name__ == "__main__":
    test_trade_buy_and_sell_side()
    test_bookticker_to_quote()
    print("\nAll Binance parsing tests passed.")
