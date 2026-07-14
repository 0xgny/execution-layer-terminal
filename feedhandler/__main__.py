"""Feedhandler entrypoint.

Usage:
    python -m feedhandler --venue binance --symbols BTCUSDT,ETHUSDT
    python -m feedhandler --venue mock          # no network required

Run the tickerplant (kdb/tp.q) and RDB (kdb/rdb.q) first; see the project README.
"""

from __future__ import annotations

import argparse
import asyncio

from .base import BaseFeedHandler
from .binance import BinanceFeedHandler
from .coinbase import CoinbaseFeedHandler
from .config import Config
from .mock import MockFeedHandler
from .publisher import TickerplantPublisher

_VENUES: dict[str, type[BaseFeedHandler]] = {
    "binance": BinanceFeedHandler,
    "coinbase": CoinbaseFeedHandler,
    "mock": MockFeedHandler,
}


def _parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Execution Layer market-data feedhandler")
    p.add_argument("--venue", choices=sorted(_VENUES), default="binance",
                   help="exchange to stream from (default: binance)")
    p.add_argument("--symbols", help="comma-separated tickers, overrides EL_SYMBOLS")
    p.add_argument("--tp-host", help="tickerplant host, overrides EL_TP_HOST")
    p.add_argument("--tp-port", type=int, help="tickerplant port, overrides EL_TP_PORT")
    return p.parse_args()


def main() -> None:
    args = _parse_args()
    config = Config()
    if args.symbols:
        config.symbols = [s.strip().upper() for s in args.symbols.split(",") if s.strip()]
    elif args.venue == "coinbase":
        # Boot the Coinbase feed on the full top-crypto universe by default.
        from .universe import TOP_CRYPTO
        config.symbols = list(TOP_CRYPTO)
    if args.tp_host:
        config.tp_host = args.tp_host
    if args.tp_port:
        config.tp_port = args.tp_port

    print(f"[main] venue={args.venue} symbols={config.symbols} "
          f"tp={config.tp_host}:{config.tp_port}")

    publisher = TickerplantPublisher(config.tp_host, config.tp_port)
    handler = _VENUES[args.venue](config, publisher)

    try:
        asyncio.run(handler.run())
    except KeyboardInterrupt:
        print("\n[main] shutting down (Ctrl-C)")
    finally:
        publisher.close()


if __name__ == "__main__":
    main()
