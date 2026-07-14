"""BinanceFeedHandler: streams live trades + top-of-book from Binance.

Uses Binance's public combined WebSocket stream. No API key is required for
market data (this is the single biggest reason we start with Binance — see
docs/decisions/0001-exchange-choice.md).

Streams consumed per symbol:
  * ``<sym>@trade``      -> individual trades            -> our `trade` table
  * ``<sym>@bookTicker`` -> best bid/ask updates (L1)    -> our `quote` table

Wire formats (combined-stream envelope is ``{"stream": ..., "data": {...}}``):

  trade data:
    { "e":"trade","E":<ms>,"s":"BTCUSDT","t":<id>,
      "p":"<price>","q":"<qty>","T":<ms>,"m":<bool> }
    ``m`` = "is the buyer the market maker?" If true, the resting order was a
    bid and the aggressor was the SELLER -> Side.SELL. Otherwise Side.BUY.

  bookTicker data (no event time is provided, so we stamp on receive):
    { "u":<updateId>,"s":"BTCUSDT",
      "b":"<bidPx>","B":"<bidQty>","a":"<askPx>","A":"<askQty>" }
"""

from __future__ import annotations

import asyncio
import json

from .base import BaseFeedHandler
from .schema import Quote, Side, Trade

_WS_BASE = "wss://stream.binance.com:9443/stream"
_MS_TO_NS = 1_000_000


class BinanceFeedHandler(BaseFeedHandler):
    exchange_name = "binance"

    def _stream_url(self) -> str:
        streams = []
        for sym in self.config.symbols:
            s = sym.lower()
            streams.append(f"{s}@trade")
            streams.append(f"{s}@bookTicker")
        return f"{_WS_BASE}?streams={'/'.join(streams)}"

    async def _run(self) -> None:
        # Imported lazily so mock/offline runs don't require the websockets dep.
        import websockets

        url = self._stream_url()
        ssl_ctx = self._build_ssl_context()
        while self._running:
            try:
                print(f"[binance] connecting: {url}")
                async with websockets.connect(url, ssl=ssl_ctx, ping_interval=20) as ws:
                    print("[binance] connected")
                    async for raw in ws:
                        self._handle_message(raw)
            except Exception as exc:  # noqa: BLE001 - reconnect on any error
                print(f"[binance] stream error: {exc!r}; reconnecting in "
                      f"{self.config.reconnect_delay_s}s")
                await asyncio.sleep(self.config.reconnect_delay_s)

    def _handle_message(self, raw: str | bytes) -> None:
        msg = json.loads(raw)
        stream = msg.get("stream", "")
        data = msg.get("data", {})
        recv = self.now_ns()

        if stream.endswith("@trade"):
            self.emit_trade(
                Trade(
                    event_time_ns=int(data["T"]) * _MS_TO_NS,
                    recv_time_ns=recv,
                    symbol=data["s"],
                    exchange=self.exchange_name,
                    price=float(data["p"]),
                    size=float(data["q"]),
                    side=Side.SELL if data["m"] else Side.BUY,
                )
            )
        elif stream.endswith("@bookTicker"):
            # bookTicker carries no timestamp; event time == receive time.
            self.emit_quote(
                Quote(
                    event_time_ns=recv,
                    recv_time_ns=recv,
                    symbol=data["s"],
                    exchange=self.exchange_name,
                    bid=float(data["b"]),
                    ask=float(data["a"]),
                    bsize=float(data["B"]),
                    asize=float(data["A"]),
                )
            )
