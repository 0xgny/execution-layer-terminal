"""MockFeedHandler: synthetic ticks with no network dependency.

Purpose: prove the entire pipeline (feedhandler -> tickerplant -> RDB -> query)
end to end without touching the internet or an exchange. It generates a plausible
random walk per symbol so downstream analytics have realistic-looking data.

It is a first-class sibling of :class:`BinanceFeedHandler` — the exact same
:class:`BaseFeedHandler` contract — which is precisely the point of the
abstraction: swapping the venue is swapping this one class.
"""

from __future__ import annotations

import asyncio
import random

from .base import BaseFeedHandler
from .schema import Quote, Side, Trade


class MockFeedHandler(BaseFeedHandler):
    exchange_name = "mock"

    def __init__(self, *args, tick_hz: float = 20.0, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._interval = 1.0 / tick_hz
        # seed a starting price per symbol
        self._px = {sym: 100.0 + random.random() * 100 for sym in self.config.symbols}

    async def _run(self) -> None:
        while self._running:
            now = self.now_ns()
            for sym in self.config.symbols:
                # random-walk the mid
                self._px[sym] = max(1.0, self._px[sym] * (1 + random.gauss(0, 0.0005)))
                mid = self._px[sym]
                spread = mid * 0.0002
                bid, ask = mid - spread / 2, mid + spread / 2

                buy = random.random() > 0.5
                self.emit_trade(
                    Trade(
                        event_time_ns=now,
                        recv_time_ns=now,
                        symbol=sym,
                        exchange=self.exchange_name,
                        price=ask if buy else bid,
                        size=round(random.expovariate(1.0), 6) + 0.001,
                        side=Side.BUY if buy else Side.SELL,
                    )
                )
                self.emit_quote(
                    Quote(
                        event_time_ns=now,
                        recv_time_ns=now,
                        symbol=sym,
                        exchange=self.exchange_name,
                        bid=bid,
                        ask=ask,
                        bsize=round(random.uniform(0.1, 5.0), 4),
                        asize=round(random.uniform(0.1, 5.0), 4),
                    )
                )
            await asyncio.sleep(self._interval)
