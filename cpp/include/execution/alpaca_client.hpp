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
// treat one instance as owned by one thread, same discipline as KdbClient).
// ============================================================================
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "execution/types.hpp"

namespace el {

class AlpacaClient {
public:
    AlpacaClient();  // reads ALPACA_API_KEY_ID / ALPACA_API_SECRET_KEY from env

    bool configured() const { return !key_.empty() && !secret_.empty(); }

    // Latest top-of-book quote for a stock symbol (IEX venue). nullopt on any
    // failure (not configured, network error, bad symbol, rate limited).
    std::optional<Quote> latest_quote(const std::string& symbol);

    // Closing prices of the last `n` daily bars, oldest first (same ordering
    // the Price Chart already expects from KdbClient::history).
    std::vector<double> daily_bars(const std::string& symbol, int n);

    const std::string& last_error() const { return err_; }

private:
    // GET `path` on the data API host with the APCA auth headers. Returns
    // false (and sets err_) on any transport or HTTP-status failure.
    bool get(const std::string& path, std::string& body);

    std::string key_;
    std::string secret_;
    std::string err_;
};

}  // namespace el
