"""FeedPublisher: pushes batches of normalized ticks into the terminal.

This is the boundary between the feedhandler and the C++ execution layer. We
take lists of :class:`Trade` / :class:`Quote` dataclasses and write them as
newline-delimited JSON over a localhost socket, where the terminal's FeedServer
decodes them straight into its in-process MarketStore.

Wire format (see cpp/include/execution/feed_server.hpp for the reading half):

    {"m":"quotes","rows":[[sym,bid,ask,bsize,asize,ts_ns], ...]}
    {"m":"trades","rows":[[sym,price,ts_ns], ...]}
    {"m":"history","sym":sym,"rows":[[price,ts_ns], ...]}   # oldest first
    {"m":"products","syms":[...]}
    {"m":"universe","syms":[...]}
    {"m":"names","map":{"BTC-USD":"Bitcoin", ...}}
    {"m":"poll"}                 -> {"m":"requested","syms":[...]}

Why batches of rows rather than one message per tick?
    Same reason the previous KDB+ publisher sent column vectors: per-message
    overhead dominates at tick rates. One flush is one line.

Connection lifecycle differs from the old tickerplant setup in one way worth
knowing: the terminal only opens its socket when you click START DESK, so the
feedhandler will usually start *before* there is anything to publish to. That is
normal. We connect lazily, retry on a backoff, and drop ticks while nothing is
attached -- exactly what the tickerplant did when it had no subscribers.
"""

from __future__ import annotations

import json
import socket
import time

from .schema import Quote, Trade

# Reconnect interval. Deliberately flat and short rather than an exponential
# backoff: the feed sits disconnected for however long it takes the user to click
# START DESK, so any backoff has already decayed to its maximum by the exact
# moment latency matters. A failed connect to a closed loopback port costs
# microseconds, and connect() is only reached from the flush loop (10x/s) and
# the control loop, so retrying every time is free.
_RETRY_INTERVAL_S = 0.25


class FeedPublisher:
    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self._sock: socket.socket | None = None
        self._rbuf = b""
        self._next_retry = 0.0
        self._announced = False
        # Catalog and streaming set are *state*, not events: the terminal needs
        # them whenever it attaches, and it usually attaches long after we send
        # them the first time (we start before it, and it only opens its socket
        # on START DESK). Cache them and resend on every connect, or the Ticker
        # Search is empty for the whole session.
        self._last_products: list[str] = []
        self._last_universe: list[str] = []
        # Latest top-of-book per symbol. Coinbase sends a snapshot for every
        # product at subscribe time and then only updates it when that product
        # trades -- so an illiquid pair may not tick again for minutes. If the
        # terminal attaches after that initial burst (which it always does; we
        # start first), those snapshots are gone and the Market Watch fills in
        # one row at a time as each product happens to trade. Caching the last
        # quote and replaying it on connect makes the watch list populate at
        # once. Bounded by symbol count, so a few hundred rows at most.
        self._last_quotes: dict[str, list] = {}
        self._last_names: dict[str, str] = {}
        self.connect()

    # -- connection ---------------------------------------------------------
    def connect(self) -> bool:
        """Try to attach to the terminal. Cheap no-op if already attached, and
        rate-limited so a disconnected feed doesn't spin on connect()."""
        if self._sock is not None:
            return True
        now = time.monotonic()
        if now < self._next_retry:
            return False
        self._next_retry = now + _RETRY_INTERVAL_S
        try:
            s = socket.create_connection((self._host, self._port), timeout=5.0)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._sock = s
            self._rbuf = b""
            self._announced = False
            print(f"[publisher] connected to terminal at {self._host}:{self._port}")
            self._resync()
            return True
        except OSError:
            if not self._announced:
                print(f"[publisher] terminal not up at {self._host}:{self._port}; "
                      "dropping ticks until it is (click START DESK in the terminal)")
                self._announced = True
            return False

    def _drop(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._rbuf = b""
        print("[publisher] terminal disconnected; will reconnect")

    def _write(self, msg: dict) -> bool:
        """Write to an already-established socket. Never calls connect(), so it's
        safe to use from inside connect() itself."""
        try:
            self._sock.sendall((json.dumps(msg, separators=(",", ":")) + "\n").encode())
            return True
        except OSError:
            self._drop()
            return False

    def _resync(self) -> None:
        """Push cached state to a freshly attached terminal: catalog, streaming
        universe, and the latest known quote for every symbol."""
        if self._last_products:
            self._write({"m": "products", "syms": self._last_products})
        if self._last_universe and self._sock is not None:
            self._write({"m": "universe", "syms": self._last_universe})
        if self._last_names and self._sock is not None:
            self._write({"m": "names", "map": self._last_names})
        if self._last_quotes and self._sock is not None:
            self._write({"m": "quotes", "rows": list(self._last_quotes.values())})

    def _send(self, msg: dict) -> bool:
        if not self.connect():
            return False
        return self._write(msg)

    # -- publish paths ------------------------------------------------------
    def publish_trades(self, trades: list[Trade]) -> None:
        if not trades:
            return
        self._send({
            "m": "trades",
            "rows": [[t.symbol, t.price, t.event_time_ns] for t in trades],
        })

    def publish_history(self, symbol: str, points: list[tuple[float, int]]) -> None:
        """Seed the terminal's chart for `symbol` with historical (price, ts_ns)
        points, oldest first.

        Not cached for resync, unlike the catalog: this is a point-in-time
        backfill that goes stale, and the terminal drops anything not older than
        the tape it already holds. A reconnecting terminal gets a fresh fetch
        from the subscribe path instead of a replay of this one.
        """
        if not points:
            return
        self._send({
            "m": "history",
            "sym": symbol,
            "rows": [[p, ts] for p, ts in points],
        })

    def publish_quotes(self, quotes: list[Quote]) -> None:
        if not quotes:
            return
        rows = [[q.symbol, q.bid, q.ask, q.bsize, q.asize, q.event_time_ns]
                for q in quotes]
        for r in rows:
            self._last_quotes[r[0]] = r   # keep the newest per symbol for resync
        self._send({"m": "quotes", "rows": rows})

    # -- control plane ------------------------------------------------------
    def query_requested(self) -> tuple[list[str], list[str]]:
        """Poll the terminal for what it wants from us.

        Returns ``(subscribe, backfill)``: symbols to start streaming, and
        symbols to fetch chart history for. They are separate lists because the
        terminal streams far more symbols than it charts -- it asks for candles
        only for the symbol on screen.
        """
        empty: tuple[list[str], list[str]] = ([], [])
        if not self._send({"m": "poll"}):
            return empty
        try:
            self._sock.settimeout(2.0)
            while b"\n" not in self._rbuf:
                chunk = self._sock.recv(8192)
                if not chunk:
                    self._drop()
                    return empty
                self._rbuf += chunk
            line, _, self._rbuf = self._rbuf.partition(b"\n")
        except OSError:
            self._drop()
            return empty
        finally:
            if self._sock is not None:
                self._sock.settimeout(None)

        try:
            msg = json.loads(line)
        except ValueError:
            return empty
        if msg.get("m") != "requested":
            return empty
        return ([str(s) for s in msg.get("syms", [])],
                [str(s) for s in msg.get("hist", [])])

    def set_products(self, ids: list[str]) -> None:
        """Publish the catalog of tradable products for the terminal to browse.
        Cached, so a terminal that attaches later still gets it."""
        self._last_products = list(ids)
        self._send({"m": "products", "syms": self._last_products})

    def set_names(self, names: dict[str, str]) -> None:
        """Publish human-readable names (BTC-USD -> "Bitcoin") for the chart
        header. Cached and resent on connect, like the rest of the catalog."""
        self._last_names = dict(names)
        self._send({"m": "names", "map": self._last_names})

    def set_universe(self, ids: list[str]) -> None:
        """Publish the set of symbols the feed is actually streaming, so the
        terminal can auto-populate its watch list at boot. Cached, as above."""
        self._last_universe = list(ids)
        self._send({"m": "universe", "syms": self._last_universe})

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:  # pragma: no cover - best-effort teardown
                pass
            self._sock = None
