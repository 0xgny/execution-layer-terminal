"""Runtime configuration for the feedhandler.

Values come from environment variables (with sensible defaults) so the same
code runs locally, in a container, or under a process manager without edits.
Nothing here is secret today -- the crypto market-data streams are public -- but
keeping config in one typed place makes it easy to add API keys later (e.g. for
Coinbase's authenticated channels or for order routing).
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field


def _env_list(name: str, default: list[str]) -> list[str]:
    raw = os.environ.get(name)
    if not raw:
        return default
    return [s.strip().upper() for s in raw.split(",") if s.strip()]


@dataclass(slots=True)
class Config:
    # --- Terminal feed socket ---------------------------------------------
    feed_host: str = os.environ.get("EL_FEED_HOST", "127.0.0.1")
    feed_port: int = int(os.environ.get("EL_FEED_PORT", "5020"))

    # --- What to stream ----------------------------------------------------
    # Symbols are exchange-native tickers (Binance uses e.g. BTCUSDT).
    symbols: list[str] = field(
        default_factory=lambda: _env_list("EL_SYMBOLS", ["BTCUSDT", "ETHUSDT"])
    )

    # --- Batching / flush behaviour ---------------------------------------
    # We buffer ticks and flush them to the terminal on a timer to amortize
    # per-message overhead, mirroring the "N ticks per publish" pattern of a
    # real feed.
    #
    # 100ms rather than the 250ms this used to default to. The batch interval is
    # the floor on end-to-end tick latency, and at 250ms the chart visibly moved
    # in four steps a second. A localhost json.dumps + sendall is microseconds,
    # so the amortization argument is just as satisfied by 100ms and the feed
    # looks continuous instead of stepped.
    flush_interval_s: float = float(os.environ.get("EL_FLUSH_INTERVAL", "0.10"))
    max_buffer: int = int(os.environ.get("EL_MAX_BUFFER", "1000"))

    # --- Reconnection ------------------------------------------------------
    reconnect_delay_s: float = float(os.environ.get("EL_RECONNECT_DELAY", "2.0"))

    # --- TLS ---------------------------------------------------------------
    # DEBUG ONLY: skip TLS certificate verification. Needed only to get past a
    # TLS-intercepting corporate proxy while developing. NEVER enable this for
    # anything that carries credentials or routes real orders.
    insecure_ssl: bool = os.environ.get("EL_INSECURE_SSL", "0") == "1"
