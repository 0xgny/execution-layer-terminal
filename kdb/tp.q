/ ============================================================================
/ tp.q  --  Tickerplant: the lightweight publish/subscribe router
/ ============================================================================
/ Role: receive updates from feedhandlers and fan them out to subscribers
/ (the RDB today; the C++ execution engine and other consumers later).
/ .

/ The tickerplant does NO analytics and NO storage of its own. Its only jobs
/ are (1) route messages and (2) — in a production system — write a replay log
/ for disaster recovery. The replay log is intentionally omitted here and left
/ as a documented next step (see docs/architecture.md).
/ .

/ Launch:  q kdb/tp.q   (listens on port 5010)
/ ============================================================================

\p 5010                      / listen for IPC connections on port 5010
\l kdb/schema.q              / load the shared table definitions

/ subscribers: maps a table name (symbol) -> list of subscriber IPC handles.
/ Using `()!()` gives an empty general dictionary we grow on demand.
subscribers:()!()

/ .u.sub[t] : called by a downstream process (e.g. the RDB) to subscribe to
/ updates for table `t`. We record the caller's handle (.z.w) once.
.u.sub:{[t]
    subscribers[t]:distinct subscribers[t],.z.w;
    -1 "[tp] handle ",(string .z.w)," subscribed to ",string t;
 }

/ .u.upd[t;data] : called by a feedhandler to publish an update.
/   t    = target table name (symbol), e.g. `trade
/   data = list of column vectors matching that table's schema
/ We forward the update asynchronously (neg h) to every subscriber of `t`.
/ Async send (neg handle) means the feedhandler is never blocked waiting on
/ a slow subscriber.
.u.upd:{[t;data]
    if[count subscribers t;
        {[h;t;data] (neg h)(`.u.upd;t;data)}[;t;data] each subscribers t ];
 }

/ .z.pc : connection-close callback. Drop dead handles from every table's
/ subscriber list so we don't try to publish to a socket that has gone away.
.z.pc:{[h]
    subscribers::(except[;h]) each subscribers;  / drop h from each list; `each` keeps keys
 }

/ ---------------------------------------------------------------------------
/ Control plane: lets the C++ terminal ask for new tickers at runtime and read
/ the catalog of available products. The feedhandler polls `requested` and
/ dynamically subscribes on the exchange; it publishes the catalog into
/ `products`. Both live here on the tickerplant because the feedhandler already
/ connects here.
/ ---------------------------------------------------------------------------
requested:`$();                              / symbols the terminal has asked for
products:`$();                               / catalog of tradable products (from feed)
.u.addsym:{[s] requested::distinct requested,s; };
.u.setproducts:{[x] products::x; };

-1 "[tp] Tickerplant up on port 5010. Waiting for feedhandler + subscribers.";
