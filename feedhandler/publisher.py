"""TickerplantPublisher: pushes batches of normalized ticks into KDB+.

This is the boundary between the Python world and the q world. We take lists of
:class:`Trade` / :class:`Quote` dataclasses, turn each field into a typed PyKX
column vector, and call ``.u.upd[table; data]`` on the tickerplant over IPC.

Why column vectors instead of row-by-row inserts?
    KDB+ is column-oriented and IPC has per-message overhead. Sending one
    ``.u.upd`` with N rows as column vectors is dramatically cheaper than N
    single-row calls. This mirrors how a production feedhandler batches ticks.

The vector *types* here must line up exactly with ``kdb/schema.q``:
    time/recv -> TimestampVector, sym/exch -> SymbolVector,
    price/size -> FloatVector, side -> CharVector.
"""

from __future__ import annotations

import numpy as np
import pykx as kx

from .schema import Quote, Trade


class TickerplantPublisher:
    def __init__(self, host: str, port: int) -> None:
        self._host = host
        self._port = port
        self.conn = kx.SyncQConnection(host=host, port=port)
        print(f"[publisher] connected to tickerplant at {host}:{port}")

    # -- helpers ------------------------------------------------------------
    @staticmethod
    def _timestamps(values_ns: list[int]) -> kx.TimestampVector:
        # numpy interprets int64 as nanoseconds since the unix epoch; PyKX then
        # converts datetime64[ns] straight into a q timestamp vector.
        arr = np.array(values_ns, dtype="datetime64[ns]")
        return kx.TimestampVector(arr)

    # -- publish paths ------------------------------------------------------
    def publish_trades(self, trades: list[Trade]) -> None:
        if not trades:
            return
        data = [
            self._timestamps([t.event_time_ns for t in trades]),
            self._timestamps([t.recv_time_ns for t in trades]),
            kx.SymbolVector([t.symbol for t in trades]),
            kx.SymbolVector([t.exchange for t in trades]),
            kx.FloatVector([t.price for t in trades]),
            kx.FloatVector([t.size for t in trades]),
            kx.CharVector("".join(t.side.value for t in trades)),
        ]
        # wait=False -> async one-way send (like `neg h` in q): the feedhandler
        # never blocks waiting for the tickerplant to acknowledge.
        self.conn(".u.upd", kx.SymbolAtom("trade"), data, wait=False)

    def publish_quotes(self, quotes: list[Quote]) -> None:
        if not quotes:
            return
        data = [
            self._timestamps([q.event_time_ns for q in quotes]),
            self._timestamps([q.recv_time_ns for q in quotes]),
            kx.SymbolVector([q.symbol for q in quotes]),
            kx.SymbolVector([q.exchange for q in quotes]),
            kx.FloatVector([q.bid for q in quotes]),
            kx.FloatVector([q.ask for q in quotes]),
            kx.FloatVector([q.bsize for q in quotes]),
            kx.FloatVector([q.asize for q in quotes]),
        ]
        self.conn(".u.upd", kx.SymbolAtom("quote"), data, wait=False)

    # -- control plane ------------------------------------------------------
    def query_requested(self) -> list[str]:
        """Symbols the terminal has asked the feed to start streaming."""
        try:
            r = self.conn("requested")
            return [str(x) for x in r.py()]
        except Exception:  # pragma: no cover - control plane is best-effort
            return []

    def set_products(self, ids: list[str]) -> None:
        """Publish the catalog of tradable products for the terminal to browse."""
        try:
            self.conn(".u.setproducts", kx.SymbolVector(ids), wait=False)
        except Exception:  # pragma: no cover
            pass

    def close(self) -> None:
        try:
            self.conn.close()
        except Exception:  # pragma: no cover - best-effort teardown
            pass
