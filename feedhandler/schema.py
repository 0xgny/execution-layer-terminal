"""Normalized tick types shared across all exchange feedhandlers.

Every exchange speaks its own wire format. Each ``BaseFeedHandler`` subclass is
responsible for translating that format into these two neutral dataclasses, so
everything downstream (the publisher, the KDB+ store, analytics, execution)
only ever sees one schema regardless of venue.

These fields map 1:1 onto the columns in ``kdb/schema.q``.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum


class Side(str, Enum):
    """Aggressor side of a trade (who crossed the spread)."""

    BUY = "B"   # buyer-initiated: aggressor lifted the ask
    SELL = "S"  # seller-initiated: aggressor hit the bid
    UNKNOWN = "U"


@dataclass(slots=True)
class Trade:
    """A single executed trade, normalized across venues."""

    event_time_ns: int  # exchange-reported time, unix nanoseconds
    recv_time_ns: int   # feedhandler receive time, unix nanoseconds
    symbol: str         # e.g. "BTCUSDT"
    exchange: str       # e.g. "binance"
    price: float
    size: float
    side: Side = Side.UNKNOWN


@dataclass(slots=True)
class Quote:
    """Top-of-book (level-1) snapshot: best bid and ask."""

    event_time_ns: int
    recv_time_ns: int
    symbol: str
    exchange: str
    bid: float
    ask: float
    bsize: float
    asize: float
