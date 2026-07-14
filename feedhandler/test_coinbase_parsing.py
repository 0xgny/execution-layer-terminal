"""Deterministic tests for Coinbase wire-format -> normalized tick parsing.

No network required. Run:  python -m feedhandler.test_coinbase_parsing
"""

from __future__ import annotations

import json

from .coinbase import CoinbaseFeedHandler, _iso_to_ns
from .config import Config
from .schema import Side


class _StubPublisher:
    """Stands in for the real publisher; never touched unless a flush fires."""


def _make_handler() -> CoinbaseFeedHandler:
    cfg = Config()
    cfg.symbols = ["BTC-USD"]
    cfg.max_buffer = 10_000  # ensure no flush during the test
    return CoinbaseFeedHandler(cfg, _StubPublisher())


def test_iso_to_ns() -> None:
    # 2014-11-07T08:19:27.028459Z -> known unix seconds 1415348367, + 28459 us
    ns = _iso_to_ns("2014-11-07T08:19:27.028459Z")
    assert ns == 1_415_348_367_028_459_000, ns
    print("ok: ISO-8601 -> unix ns")


def test_match_aggressor_inversion() -> None:
    h = _make_handler()

    # maker side "sell" -> aggressor BUY
    m_sell = {"type": "match", "trade_id": 1, "time": "2020-01-01T00:00:00.000000Z",
              "product_id": "BTC-USD", "size": "0.5", "price": "9000.00", "side": "sell"}
    # maker side "buy" -> aggressor SELL
    m_buy = {"type": "match", "trade_id": 2, "time": "2020-01-01T00:00:01.000000Z",
             "product_id": "BTC-USD", "size": "1.25", "price": "8999.00", "side": "buy"}

    h._handle_message(json.dumps(m_sell))
    h._handle_message(json.dumps(m_buy))

    assert len(h._trades) == 2, h._trades
    t0, t1 = h._trades
    assert t0.side is Side.BUY, "maker=sell should map to aggressor BUY"
    assert t1.side is Side.SELL, "maker=buy should map to aggressor SELL"
    assert t0.price == 9000.00 and t0.size == 0.5
    assert t0.symbol == "BTC-USD" and t0.exchange == "coinbase"
    print("ok: match parsing + maker->aggressor inversion")


def test_ticker_to_quote() -> None:
    h = _make_handler()
    ticker = {"type": "ticker", "product_id": "BTC-USD", "price": "9000.5",
              "time": "2020-01-01T00:00:00.000000Z",
              "best_bid": "9000.00", "best_bid_size": "2.0",
              "best_ask": "9001.00", "best_ask_size": "3.0"}
    # snapshot without a book should be ignored
    h._handle_message(json.dumps({"type": "ticker", "product_id": "BTC-USD"}))
    h._handle_message(json.dumps(ticker))

    assert len(h._quotes) == 1, h._quotes
    q = h._quotes[0]
    assert q.bid == 9000.00 and q.ask == 9001.00
    assert q.bsize == 2.0 and q.asize == 3.0
    print("ok: ticker -> quote (and snapshot-without-book ignored)")


if __name__ == "__main__":
    test_iso_to_ns()
    test_match_aggressor_inversion()
    test_ticker_to_quote()
    print("\nAll Coinbase parsing tests passed.")
