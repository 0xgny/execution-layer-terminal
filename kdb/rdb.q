/ ============================================================================
/ rdb.q  --  Real-Time Database: in-memory intraday tick store + query surface
/ ============================================================================
/ Role: subscribe to the tickerplant, append every incoming update into local
/ in-memory `trade` / `quote` tables, and answer low-latency read queries.
/ .

/ There is currently NO end-of-day flush to a partitioned on-disk HDB; data
/ lives only for the life of this process. Persisting to an HDB is the next
/ planned phase (see docs/architecture.md, "Roadmap").
/ .

/ Launch:  q kdb/rdb.q   (listens on 5011, connects out to the tp on 5010)
/ ============================================================================

\p 5011                      / expose a query port for analytics / execution
\l kdb/schema.q              / same table definitions as the tickerplant

/ Connect to the tickerplant. `hopen` returns a handle we use to subscribe.
tph:hopen `::5010

/ .u.upd[t;data] : the tickerplant calls this on us for every published update.
/ We simply append the column vectors into the matching local table.
.u.upd:{[t;data] t insert data; }

/ Subscribe to the tables we care about.
tph(`.u.sub;`trade);
tph(`.u.sub;`quote);

/ ---------------------------------------------------------------------------
/ Convenience introspection helpers (handy while developing / from the q REPL)
/ ---------------------------------------------------------------------------
/ counts[] -> row counts per table, plus mean ingestion latency in ms.
counts:{
    `trades`quotes`avg_lat_ms!(
        count trade;
        count quote;
        $[count trade; avg (`float$(trade`recv)-trade`time)%1e6; 0n] )
 }

/ snap[syms] -> unkeyed table, one row per requested symbol: latest top-of-book
/ plus last trade price. This is the flat, column-oriented shape the C++ terminal
/ polls over IPC (easy to parse with the k.h accessors). `price` is null until a
/ symbol has printed a trade.
snap:{[syms]
    qt:select last bid, last ask, last bsize, last asize by sym from quote where sym in syms;
    tt:select last price by sym from trade where sym in syms;
    0!(qt lj tt) }

/ hist[sym;n] -> the last n trade prices for a symbol (a float vector). Used by
/ the C++ terminal to draw a live price chart.
hist:{[s;n] neg[n]#exec price from trade where sym=s}

-1 "[rdb] RDB up on port 5011, subscribed to tickerplant. Ready for queries.";
