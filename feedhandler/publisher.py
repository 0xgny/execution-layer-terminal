"""FeedPublisher: pushes batches of normalized ticks into the terminal.

This is the boundary between the feedhandler and the C++ execution layer. We
take lists of :class:`Trade` / :class:`Quote` dataclasses and write them as
newline-delimited JSON over a localhost socket, where the terminal's FeedServer
decodes them straight into its in-process MarketStore.

Wire format (see cpp/include/execution/feed_server.hpp for the reading half):

    {"m":"quotes","rows":[[sym,bid,ask,bsize,asize,ts_ns], ...]}
    {"m":"trades","rows":[[sym,price,ts_ns], ...]}
    {"m":"products","syms":[...]}
    {"m":"universe","syms":[...]}
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

_RETRY_INTERVAL_S = 2.0


class FeedPublisher:
    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self._sock: socket.socket | None = None
        self._rbuf = b""
        self._next_retry = 0.0
        self._announced = False
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

    def _send(self, msg: dict) -> bool:
        if not self.connect():
            return False
        try:
            self._sock.sendall((json.dumps(msg, separators=(",", ":")) + "\n").encode())
            return True
        except OSError:
            self._drop()
            return False

    # -- publish paths ------------------------------------------------------
    def publish_trades(self, trades: list[Trade]) -> None:
        if not trades:
            return
        self._send({
            "m": "trades",
            "rows": [[t.symbol, t.price, t.event_time_ns] for t in trades],
        })

    def publish_quotes(self, quotes: list[Quote]) -> None:
        if not quotes:
            return
        self._send({
            "m": "quotes",
            "rows": [[q.symbol, q.bid, q.ask, q.bsize, q.asize, q.event_time_ns]
                     for q in quotes],
        })

    # -- control plane ------------------------------------------------------
    def query_requested(self) -> list[str]:
        """Symbols the terminal has asked the feed to start streaming."""
        if not self._send({"m": "poll"}):
            return []
        try:
            self._sock.settimeout(2.0)
            while b"\n" not in self._rbuf:
                chunk = self._sock.recv(8192)
                if not chunk:
                    self._drop()
                    return []
                self._rbuf += chunk
            line, _, self._rbuf = self._rbuf.partition(b"\n")
        except OSError:
            self._drop()
            return []
        finally:
            if self._sock is not None:
                self._sock.settimeout(None)

        try:
            msg = json.loads(line)
        except ValueError:
            return []
        if msg.get("m") != "requested":
            return []
        return [str(s) for s in msg.get("syms", [])]

    def set_products(self, ids: list[str]) -> None:
        """Publish the catalog of tradable products for the terminal to browse."""
        self._send({"m": "products", "syms": list(ids)})

    def set_universe(self, ids: list[str]) -> None:
        """Publish the set of symbols the feed is actually streaming, so the
        terminal can auto-populate its watch list at boot."""
        self._send({"m": "universe", "syms": list(ids)})

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:  # pragma: no cover - best-effort teardown
                pass
            self._sock = None
