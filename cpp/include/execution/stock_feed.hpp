// ============================================================================
// stock_feed.hpp -- Background poller for Alpaca stock quotes.
//
// HTTP round-trips (tens to hundreds of ms) are too slow to run on
// TradingEngine's 30ms loop without stalling crypto polling too. StockFeed
// owns its own thread, exactly like TradingEngine owns FeedServer, and
// publishes a small thread-safe cache that the engine reads each cycle with a
// cheap mutex copy -- no I/O on the engine thread for stocks.
//
// Rate budget: Alpaca's free plan allows 200 requests/minute. Every watched
// symbol is refreshed in ONE multi-symbol request, so steady-state load is a
// flat 60 req/min at kQuoteIntervalMs=1000 no matter how long the watch list
// gets. The old shape polled each symbol separately, which forced a 10s
// per-symbol interval to stay inside the budget.
//
// The chart series is sampled here, not derived from a trade tape: Alpaca's
// free plan gives us quotes, not a live trade stream, so `history()` is the
// per-poll mid of each watched symbol. That is a real intraday time series and
// it advances every poll. It replaced daily bars, which were what the Price
// Chart used to plot -- 120 *daily* closes cannot move during a session, so the
// stock chart was structurally incapable of animating.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
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
    // Points retained per symbol: the minute-bar backfill plus everything
    // sampled live since. Deep enough to hold a full session of backfill and
    // still leave room for hours of 1/s sampling on top.
    static constexpr std::size_t kHistoryCap = 4096;

    // Minute bars fetched when a symbol is first watched -- a full regular
    // session is 390 minutes, so this is "today, plus a bit of yesterday".
    static constexpr int kBackfillMinutes = 480;

    StockFeed() = default;
    ~StockFeed();

    void start();
    void stop();

    bool configured() const { return client_.configured(); }

    // Thread-safe: start watching a symbol (idempotent). It joins the next
    // multi-symbol poll, so its first quote lands within kQuoteIntervalMs.
    void watch(const std::string& symbol);

    // Thread-safe cached reads.
    std::map<std::string, Quote> snapshot();

    // Intraday price points for `symbol`, oldest first -- same shape and
    // ordering the Price Chart gets from MarketStore::history for crypto.
    // Minute bars up to the moment the symbol was added, then one sampled mid
    // per poll after that.
    std::vector<PricePoint> history(const std::string& symbol);

    // Company name for a watched symbol, or empty until the poller has fetched
    // it. Cosmetic: the chart falls back to the bare symbol.
    std::string name(const std::string& symbol);

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
    std::map<std::string, std::deque<PricePoint>> history_;
    std::map<std::string, std::string> names_;
    std::map<std::string, bool> backfilled_;  // symbol -> backfill attempted

    std::mutex err_mu_;
    std::deque<std::string> errors_;
};

}  // namespace el
