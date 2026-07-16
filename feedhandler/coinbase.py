"""CoinbaseFeedHandler: streams live trades + top-of-book from Coinbase.

Uses Coinbase's public Exchange WebSocket feed (``ws-feed.exchange.coinbase.com``).
The ``matches`` and ``ticker`` channels are public and require no authentication,
which keeps this a drop-in sibling of the Binance handler. (The ``level2`` /
order-book channels require a signed JWT -- deliberately out of scope here; see
architecture.md.)

Symbol format note: Coinbase product ids are dash-separated quote pairs, e.g.
``BTC-USD`` / ``ETH-USD`` -- NOT Binance's ``BTCUSDT``. Pass venue-native symbols:
    python -m feedhandler --venue coinbase --symbols BTC-USD,ETH-USD

Channels consumed:
  * ``matches`` -> individual trades            -> our `trade` table
  * ``ticker``  -> best bid/ask (updated on trades) -> our `quote` table

Wire formats:

  subscribe (sent on connect):
    { "type":"subscribe", "product_ids":["BTC-USD"], "channels":["matches","ticker"] }

  match / last_match:
    { "type":"match","trade_id":10,"time":"2014-11-07T08:19:27.028459Z",
      "product_id":"BTC-USD","size":"5.23","price":"400.23","side":"sell" }
    NOTE: ``side`` is the side of the MAKER (resting) order. If the maker was a
    seller, the aggressor was a buyer -> Side.BUY, and vice versa. So we INVERT it.

  ticker:
    { "type":"ticker","product_id":"BTC-USD","price":"...","time":"...Z",
      "best_bid":"...","best_bid_size":"...","best_ask":"...","best_ask_size":"..." }
"""

from __future__ import annotations

import asyncio
import json
from datetime import datetime, timezone

from .base import BaseFeedHandler
from .schema import Quote, Side, Trade

_WS_URL = "wss://ws-feed.exchange.coinbase.com"
_PRODUCTS_URL = "https://api.exchange.coinbase.com/products"
_EPOCH = datetime(1970, 1, 1, tzinfo=timezone.utc)


def _iso_to_ns(ts: str) -> int:
    """Parse a Coinbase ISO-8601 UTC timestamp to unix nanoseconds (us precision)."""
    # e.g. "2014-11-07T08:19:27.028459Z" -- fromisoformat handles the trailing Z on 3.11+
    dt = datetime.fromisoformat(ts)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    delta = dt - _EPOCH
    return (delta.days * 86_400 + delta.seconds) * 1_000_000_000 + delta.microseconds * 1_000


def _subscribe_msg(product_ids: list[str]) -> str:
    return json.dumps({
        "type": "subscribe",
        "product_ids": product_ids,
        "channels": ["matches", "ticker"],
    })


class CoinbaseFeedHandler(BaseFeedHandler):
    exchange_name = "coinbase"
    _ws = None       # class default so the control loop can safely check it early
    _catalog = None  # set of valid product ids (once the catalog is fetched)

    async def _run(self) -> None:
        # Imported lazily so mock/offline runs don't require the websockets dep.
        import websockets

        ssl_ctx = self._build_ssl_context()
        await self._publish_catalog(ssl_ctx)

        # Drop any boot symbols that aren't in the live catalog (delisted, typo'd).
        if self._catalog:
            valid = [s for s in self._subscribed if s in self._catalog]
            dropped = len(self._subscribed) - len(valid)
            self._subscribed = set(valid)
            if dropped:
                print(f"[coinbase] dropped {dropped} boot symbols not in catalog")

        while self._running:
            try:
                prods = sorted(self._subscribed)
                print(f"[coinbase] connecting: {len(prods)} products")
                async with websockets.connect(_WS_URL, ssl=ssl_ctx, ping_interval=20,
                                              max_queue=2048) as ws:
                    self._ws = ws
                    # (re)subscribe to everything we currently want on (re)connect
                    await ws.send(_subscribe_msg(prods))
                    self.publisher.set_universe(prods)
                    print(f"[coinbase] connected + subscribed to {len(prods)} products")
                    async for raw in ws:
                        self._handle_message(raw)
            except Exception as exc:  # noqa: BLE001 - reconnect on any error
                print(f"[coinbase] stream error: {exc!r}; reconnecting in "
                      f"{self.config.reconnect_delay_s}s")
                await asyncio.sleep(self.config.reconnect_delay_s)
            finally:
                self._ws = None

    async def _subscribe_new(self, syms: list[str]) -> None:
        # Coinbase treats each subscribe message additively, so we can add
        # products to the live socket at runtime.
        if self._ws is not None:
            await self._ws.send(_subscribe_msg(syms))
            self.publisher.set_universe(sorted(set(self._subscribed) | set(syms)))

    async def _publish_catalog(self, ssl_ctx) -> None:
        """Fetch the list of tradable USD/USDC products and publish it so the
        terminal can offer a searchable ticker list."""
        import asyncio as _asyncio
        import urllib.request

        def fetch() -> list[str]:
            req = urllib.request.Request(_PRODUCTS_URL, headers={"User-Agent": "execution-layer"})
            with urllib.request.urlopen(req, timeout=10, context=ssl_ctx) as r:
                data = json.loads(r.read().decode())
            return sorted(
                p["id"] for p in data
                if p.get("status") == "online"
                and not p.get("trading_disabled", False)
                and p.get("quote_currency") in ("USD", "USDC")
            )

        try:
            loop = _asyncio.get_running_loop()
            ids = await loop.run_in_executor(None, fetch)
            self._catalog = set(ids)
            self.publisher.set_products(ids)
            print(f"[coinbase] published catalog: {len(ids)} USD/USDC products")
        except Exception as exc:  # noqa: BLE001
            print(f"[coinbase] catalog fetch failed: {exc!r}")

    def _handle_message(self, raw: str | bytes) -> None:
        msg = json.loads(raw)
        mtype = msg.get("type")
        recv = self.now_ns()

        if mtype in ("match", "last_match"):
            # Invert the maker side to get the aggressor side.
            aggressor = Side.BUY if msg["side"] == "sell" else Side.SELL
            self.emit_trade(
                Trade(
                    event_time_ns=_iso_to_ns(msg["time"]),
                    recv_time_ns=recv,
                    symbol=msg["product_id"],
                    exchange=self.exchange_name,
                    price=float(msg["price"]),
                    size=float(msg["size"]),
                    side=aggressor,
                )
            )
        elif mtype == "ticker":
            # Some ticker messages (e.g. the initial snapshot) may omit best_bid/ask.
            if "best_bid" not in msg or "best_ask" not in msg:
                return
            self.emit_quote(
                Quote(
                    event_time_ns=_iso_to_ns(msg["time"]) if "time" in msg else recv,
                    recv_time_ns=recv,
                    symbol=msg["product_id"],
                    exchange=self.exchange_name,
                    bid=float(msg["best_bid"]),
                    ask=float(msg["best_ask"]),
                    bsize=float(msg.get("best_bid_size", 0.0)),
                    asize=float(msg.get("best_ask_size", 0.0)),
                )
            )
        # ignore: subscriptions, heartbeat, error, etc.
