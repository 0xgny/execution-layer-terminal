// ============================================================================
// alpaca_client.hpp -- Minimal REST client for Alpaca's free market-data plan.
//
// Alpaca's "Basic" plan (free) gives real IEX-venue real-time quotes and
// historical daily bars, no live order routing needed here -- this app already
// has its own paper OMS (see oms.hpp). This client only ever reads market data.
//
// Auth: two env vars, read once at construction --
//   ALPACA_API_KEY_ID, ALPACA_API_SECRET_KEY
// If either is unset, configured() is false and every call is a no-op that
// returns nullopt/empty. The rest of the terminal (crypto) is unaffected.
//
// Threading: not thread-safe by itself (each call opens its own libcurl easy
// handle, so concurrent calls from different threads are fine in practice, but
// treat one instance as owned by one thread).
// ============================================================================
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "execution/types.hpp"

namespace el {

class AlpacaClient {
public:
    AlpacaClient();  // reads ALPACA_API_KEY_ID / ALPACA_API_SECRET_KEY from env

    bool configured() const { return !key_.empty() && !secret_.empty(); }

    // Latest top-of-book for many symbols in ONE request. Alpaca's multi-symbol
    // endpoint costs the same one request whether you ask for 1 symbol or 50, so
    // the poller's rate budget stops scaling with the size of the watch list --
    // that's what lets StockFeed refresh every second instead of every ten.
    // Symbols with no quote in the response are simply absent from the map;
    // empty map (and last_error() set) on any transport/HTTP failure.
    std::map<std::string, Quote> latest_quotes(const std::vector<std::string>& symbols);

    // Single-symbol convenience wrapper over latest_quotes(). nullopt on any
    // failure (not configured, network error, bad symbol, rate limited).
    std::optional<Quote> latest_quote(const std::string& symbol);

    // Closing prices of the last `n` daily bars, oldest first (same ordering
    // the Price Chart already expects from MarketStore::history).
    std::vector<double> daily_bars(const std::string& symbol, int n);

    // The most recent `n` one-minute bar closes, oldest first, timestamped.
    // This is the Price Chart's backfill: a stock the user just added would
    // otherwise start from an empty series and take minutes of 1/s sampling to
    // become a readable line. One request, and the chart has the session's
    // shape immediately.
    std::vector<PricePoint> recent_minute_bars(const std::string& symbol, int n);

    // Company name for a symbol ("AAPL" -> "Apple Inc. Common Stock"), for the
    // chart header. Empty on any failure -- purely cosmetic, never fatal.
    // Note this lives on the *trading* host, not the market-data host.
    std::string asset_name(const std::string& symbol);

    const std::string& last_error() const { return err_; }

private:
    // GET `path` on `host` with the APCA auth headers. Returns false (and sets
    // err_) on any transport or HTTP-status failure.
    static constexpr const char* kDataHost = "https://data.alpaca.markets";
    // Asset metadata lives on the trading API, not the data API. paper-api
    // works with the free/paper keys this app expects; api.alpaca.markets
    // returns 401 for them.
    static constexpr const char* kTradingHost = "https://paper-api.alpaca.markets";

    bool get(const std::string& path, std::string& body, const char* host = kDataHost);

    std::string key_;
    std::string secret_;
    std::string err_;
};

}  // namespace el
