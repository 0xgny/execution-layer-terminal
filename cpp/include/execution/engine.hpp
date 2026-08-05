// ============================================================================
// engine.hpp -- TradingEngine: the threaded core behind the terminal.
//
// Threading model (the fast/clean part):
//   * ONE background thread owns all mutable trading state (Portfolio, OMS,
//     quote cache) and drives the whole desk.
//   * The GUI thread never touches that state directly. It POSTs Commands
//     (buy/sell/flatten/...) into a queue and reads an immutable EngineView
//     snapshot that the engine republishes each cycle. Two small mutexes guard
//     the command queue and the published view; the render loop never blocks.
//   * Market data arrives on two other threads -- FeedServer (crypto) and
//     StockFeed (Alpaca) -- each writing into its own thread-safe cache that
//     the engine reads. No network I/O happens on the engine thread at all.
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "execution/feed_process.hpp"
#include "execution/feed_server.hpp"
#include "execution/market_store.hpp"
#include "execution/matching.hpp"
#include "execution/oms.hpp"
#include "execution/portfolio.hpp"
#include "execution/risk.hpp"
#include "execution/stock_feed.hpp"
#include "execution/types.hpp"

namespace el {

// Per-symbol row shown in the UI: live top-of-book + our position in it.
struct SymbolView {
    std::string symbol;
    AssetClass asset_class = AssetClass::Crypto;
    bool has_quote = false;
    double bid = 0, ask = 0, mid = 0, bsize = 0, asize = 0;
    double net_qty = 0, avg_price = 0, unrealized = 0, realized = 0;
};

// A fully closed round-trip, shown in Positions > Previous.
struct ClosedPositionView {
    std::string symbol;
    AssetClass asset_class = AssetClass::Crypto;
    double qty = 0;            // size that was closed
    double avg_entry = 0;
    double exit_price = 0;
    double realized_pnl = 0;   // pnl of this round trip only
    TimestampNs closed_ts_ns = 0;
};

// Immutable snapshot the GUI renders. Copied out under a mutex each frame.
struct EngineView {
    bool connected = false;
    bool risk_killed = false;
    std::string status;
    double initial_capital = 0, cash = 0, equity = 0;
    double total_pnl = 0, realized = 0, unrealized = 0;
    std::vector<SymbolView> symbols;
    std::vector<ClosedPositionView> closed_positions;  // newest first
    std::vector<std::string> log;       // most-recent last
    std::vector<std::string> products;  // catalog for the ticker search (crypto)
    std::string focus;                  // symbol whose chart is populated
    std::string focus_name;             // e.g. "Bitcoin"; empty if unknown
    std::vector<double> price_history;  // last-trade prices for `focus`
    std::vector<double> pnl_history;    // account PnL over time
    std::vector<double> equity_history; // account equity over time
    bool stocks_enabled = false;        // true once Alpaca credentials are set
};

// Command posted from the GUI to the engine thread.
struct Command {
    enum Type { Buy, Sell, Flatten, AddSymbol, Kill, SetFocus } type;
    std::string symbol;
    double amount = 0;          // notional ($) when use_notional, else quantity
    bool use_notional = true;
    AssetClass asset_class = AssetClass::Crypto;  // routes AddSymbol
};

class TradingEngine {
public:
    // `feed_port` is the localhost port the Python feedhandler publishes into.
    TradingEngine(int feed_port, std::vector<std::string> initial_symbols,
                  double initial_capital, RiskLimits limits);
    ~TradingEngine();

    void start();
    void stop();

    void post(const Command& c);  // thread-safe
    EngineView view();            // thread-safe snapshot copy

private:
    void run();                   // background loop body
    void process(const Command& c);
    void log(const std::string& s);
    void publish();
    bool is_stock(const std::string& symbol) const;

    std::vector<std::string> symbols_;  // engine-thread owned
    double initial_capital_;

    MarketStore store_;   // crypto quotes/history/catalog (declare before feed_)
    FeedServer feed_;     // accepts the Python feedhandler, own thread
    FeedProcess feed_proc_;  // launches that feedhandler when we're a .app
    StockFeed stocks_;    // Alpaca poller, own thread (stocks)
    Portfolio pf_;
    RiskManager risk_;
    PaperMatchingEngine matcher_;
    OrderManager oms_;
    std::map<std::string, Quote> quotes_;
    std::vector<std::string> stock_symbols_;  // engine-thread owned, parallel to symbols_

    std::string focus_;                      // symbol charted in the UI
    std::vector<std::string> products_;      // cached catalog
    std::vector<double> price_hist_;         // last-trade prices for focus_
    std::deque<double> pnl_hist_, eq_hist_;  // account time series (ring)

    // Named refresh deadlines. Previously `cycle_ % 15` / `% 66`, which encoded
    // these intervals as counts of the loop sleep -- change the sleep and both
    // silently meant something else.
    std::chrono::steady_clock::time_point next_history_{};
    std::chrono::steady_clock::time_point next_catalog_{};
    bool universe_seen_ = false;  // keep polling fast until the watch list lands

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex cmd_mu_;
    std::deque<Command> cmds_;

    std::mutex view_mu_;
    EngineView view_;
    std::deque<std::string> log_;
};

}  // namespace el
