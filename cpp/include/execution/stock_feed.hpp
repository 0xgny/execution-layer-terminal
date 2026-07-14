// ============================================================================
// stock_feed.hpp -- Background poller for Alpaca stock quotes.
//
// HTTP round-trips (tens to hundreds of ms) are too slow to run on
// TradingEngine's 30ms loop without stalling crypto polling too. StockFeed
// owns its own thread, exactly like TradingEngine owns KdbClient, and
// publishes a small thread-safe cache that the engine reads each cycle with a
// cheap mutex copy -- no I/O on the engine thread for stocks.
//
// Rate budget: Alpaca's free plan allows 200 requests/minute. Refreshing each
// watched symbol at most once every kQuoteRefreshSecs, round-robin, keeps
// steady-state load at (symbols / kQuoteRefreshSecs) req/s -- comfortably
// under budget even with dozens of symbols watched.
// ============================================================================
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "execution/alpaca_client.hpp"
#include "execution/types.hpp"

namespace el {

class StockFeed {
public:
    StockFeed() = default;
    ~StockFeed();

    void start();
    void stop();

    bool configured() const { return client_.configured(); }

    // Thread-safe: start watching a symbol (idempotent). Kicks off an
    // immediate bars fetch on the feed thread so the chart has shape right away.
    void watch(const std::string& symbol);

    // Thread-safe cached reads.
    std::map<std::string, Quote> snapshot();
    std::vector<double> history(const std::string& symbol);

    // Drains queued error strings (network/auth/parse failures) for the
    // engine to fold into its own event log.
    std::vector<std::string> errors();

private:
    void run();
    void note_error(const std::string& e);

    AlpacaClient client_;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex sym_mu_;
    std::vector<std::string> symbols_;              // feed-thread + GUI-thread shared

    std::mutex data_mu_;
    std::map<std::string, Quote> quotes_;
    std::map<std::string, std::vector<double>> bars_;
    std::map<std::string, TimestampNs> last_quote_fetch_;
    std::map<std::string, TimestampNs> last_bars_fetch_;

    std::mutex err_mu_;
    std::deque<std::string> errors_;
};

}  // namespace el
