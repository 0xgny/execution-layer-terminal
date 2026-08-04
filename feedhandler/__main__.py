"""Feedhandler entrypoint.

Usage:
    python -m feedhandler --venue binance --symbols BTCUSDT,ETHUSDT
    python -m feedhandler --venue mock          # no network required

Publishes into the C++ terminal over a localhost socket. The terminal can be
started before or after this process; see the project README.
"""

from __future__ import annotations

import argparse
import asyncio

from .base import BaseFeedHandler
from .binance import BinanceFeedHandler
from .coinbase import CoinbaseFeedHandler
from .config import Config
from .mock import MockFeedHandler
from .publisher import FeedPublisher

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
    p.add_argument("--feed-host", help="terminal host, overrides EL_FEED_HOST")
    p.add_argument("--feed-port", type=int, help="terminal port, overrides EL_FEED_PORT")
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
    if args.feed_host:
        config.feed_host = args.feed_host
    if args.feed_port:
        config.feed_port = args.feed_port

    print(f"[main] venue={args.venue} symbols={config.symbols} "
          f"terminal={config.feed_host}:{config.feed_port}")

    publisher = FeedPublisher(config.feed_host, config.feed_port)
    handler = _VENUES[args.venue](config, publisher)

    try:
        asyncio.run(handler.run())
    except KeyboardInterrupt:
        print("\n[main] shutting down (Ctrl-C)")
    finally:
        publisher.close()


if __name__ == "__main__":
    main()
