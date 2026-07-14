"""BaseFeedHandler: shared machinery for every exchange feed.

Responsibilities that live here (so subclasses don't repeat them):
  * buffering trades/quotes and flushing them to the tickerplant on a timer
  * running the flush in a thread executor so the blocking PyKX IPC call never
    stalls the asyncio event loop that reads the socket
  * lifecycle / graceful shutdown

A concrete subclass only implements :meth:`_run`, whose job is to read from its
exchange and call :meth:`emit_trade` / :meth:`emit_quote` with normalized ticks.
This is the abstraction that lets us add Coinbase later as a sibling class
without touching anything downstream — see docs/decisions/0001-exchange-choice.md
"""

from __future__ import annotations

import abc
import asyncio
import time

from .config import Config
from .publisher import TickerplantPublisher
from .schema import Quote, Trade


class BaseFeedHandler(abc.ABC):
    #: short venue tag written into every tick's ``exch`` column
    exchange_name: str = "base"

    def __init__(self, config: Config, publisher: TickerplantPublisher) -> None:
        self.config = config
        self.publisher = publisher
        self._trades: list[Trade] = []
        self._quotes: list[Quote] = []
        self._running = False
        # running totals for the periodic stats line
        self._total_trades = 0
        self._total_quotes = 0
        # symbols currently streaming (seeded from config; grows via control plane)
        self._subscribed: set[str] = set(config.symbols)

    # -- called by subclasses ----------------------------------------------
    def emit_trade(self, trade: Trade) -> None:
        self._trades.append(trade)
        if len(self._trades) >= self.config.max_buffer:
            self._flush()

    def emit_quote(self, quote: Quote) -> None:
        self._quotes.append(quote)
        if len(self._quotes) >= self.config.max_buffer:
            self._flush()

    @staticmethod
    def now_ns() -> int:
        """Wall-clock receive timestamp in unix nanoseconds."""
        return time.time_ns()

    def _build_ssl_context(self):
        """Shared TLS context for any WebSocket venue.

        Uses certifi's CA bundle so we don't depend on the OS/Python cert store
        being provisioned (a common macOS gotcha). ``EL_INSECURE_SSL=1`` disables
        verification — DEBUG ONLY, to get past a TLS-intercepting proxy; never for
        anything carrying credentials or routing real orders.
        """
        import ssl

        if self.config.insecure_ssl:
            print(f"[{self.exchange_name}] WARNING: TLS verification DISABLED "
                  "(EL_INSECURE_SSL=1) — debug only, never for real order routing")
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            return ctx
        import certifi

        return ssl.create_default_context(cafile=certifi.where())

    # -- flush machinery ----------------------------------------------------
    def _flush(self) -> None:
        """Swap out the buffers and ship them. Runs on the executor thread."""
        if self._trades:
            trades, self._trades = self._trades, []
            self.publisher.publish_trades(trades)
            self._total_trades += len(trades)
        if self._quotes:
            quotes, self._quotes = self._quotes, []
            self.publisher.publish_quotes(quotes)
            self._total_quotes += len(quotes)

    async def _flush_loop(self) -> None:
        while self._running:
            await asyncio.sleep(self.config.flush_interval_s)
            # Flush on THIS (event-loop / main) thread. PyKX's embedded q is not
            # thread-safe, so we must never publish from a worker thread. Sends
            # are fire-and-forget (wait=False in the publisher), so the blocking
            # time here is just serialize-and-write — negligible at these rates.
            self._flush()

    async def _stats_loop(self) -> None:
        while self._running:
            await asyncio.sleep(5.0)
            print(
                f"[{self.exchange_name}] published "
                f"{self._total_trades} trades / {self._total_quotes} quotes"
            )

    async def _control_loop(self) -> None:
        """Poll the tickerplant for terminal-requested symbols and subscribe to
        any that aren't streaming yet. This is the control plane that lets the
        C++ terminal add arbitrary tickers at runtime."""
        while self._running:
            await asyncio.sleep(2.0)
            requested = self.publisher.query_requested()
            new = [s for s in requested if s and s not in self._subscribed]
            if new:
                try:
                    await self._subscribe_new(new)
                    self._subscribed.update(new)
                    print(f"[{self.exchange_name}] now streaming {new}")
                except Exception as exc:  # noqa: BLE001
                    print(f"[{self.exchange_name}] subscribe failed for {new}: {exc!r}")

    async def _subscribe_new(self, syms: list[str]) -> None:
        """Subscribe to additional symbols on an already-open stream.
        Default: no-op (override per venue)."""
        _ = syms

    # -- subclass contract --------------------------------------------------
    @abc.abstractmethod
    async def _run(self) -> None:
        """Read from the exchange forever, calling emit_trade / emit_quote."""

    # -- entrypoint ---------------------------------------------------------
    async def run(self) -> None:
        self._running = True
        try:
            await asyncio.gather(self._run(), self._flush_loop(), self._stats_loop(),
                                 self._control_loop())
        finally:
            self._running = False
            self._flush()  # final drain
