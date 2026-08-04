"""Execution Layer feedhandler package.

Normalizes exchange market-data streams into a single (trade, quote) schema and
publishes them into the C++ terminal over a localhost socket.
"""

from .base import BaseFeedHandler
from .binance import BinanceFeedHandler
from .coinbase import CoinbaseFeedHandler
from .config import Config
from .mock import MockFeedHandler
from .publisher import FeedPublisher
from .schema import Quote, Side, Trade

__all__ = [
    "BaseFeedHandler",
    "BinanceFeedHandler",
    "CoinbaseFeedHandler",
    "MockFeedHandler",
    "FeedPublisher",
    "Config",
    "Trade",
    "Quote",
    "Side",
]
