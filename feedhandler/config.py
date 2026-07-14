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
    # --- Tickerplant IPC target -------------------------------------------
    tp_host: str = os.environ.get("EL_TP_HOST", "localhost")
    tp_port: int = int(os.environ.get("EL_TP_PORT", "5010"))

    # --- What to stream ----------------------------------------------------
    # Symbols are exchange-native tickers (Binance uses e.g. BTCUSDT).
    symbols: list[str] = field(
        default_factory=lambda: _env_list("EL_SYMBOLS", ["BTCUSDT", "ETHUSDT"])
    )

    # --- Batching / flush behaviour ---------------------------------------
    # We buffer ticks and flush them to the tickerplant on a timer to amortize
    # IPC overhead, mirroring the "N ticks per publish" pattern of a real feed.
    flush_interval_s: float = float(os.environ.get("EL_FLUSH_INTERVAL", "0.25"))
    max_buffer: int = int(os.environ.get("EL_MAX_BUFFER", "1000"))

    # --- Reconnection ------------------------------------------------------
    reconnect_delay_s: float = float(os.environ.get("EL_RECONNECT_DELAY", "2.0"))

    # --- TLS ---------------------------------------------------------------
    # DEBUG ONLY: skip TLS certificate verification. Needed only to get past a
    # TLS-intercepting corporate proxy while developing. NEVER enable this for
    # anything that carries credentials or routes real orders.
    insecure_ssl: bool = os.environ.get("EL_INSECURE_SSL", "0") == "1"
