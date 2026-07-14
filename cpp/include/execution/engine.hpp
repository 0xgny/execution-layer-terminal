// ============================================================================
// engine.hpp -- TradingEngine: the threaded core behind the terminal.
//
// Threading model (the fast/clean part):
//   * ONE background thread owns all mutable trading state (KdbClient, Portfolio,
//     OMS, quote cache) and does all KDB+ IPC. KDB+/PyKX client state is not
//     thread-safe, so it lives on exactly one thread.
//   * The GUI thread never touches that state directly. It POSTs Commands
//     (buy/sell/flatten/...) into a queue and reads an immutable EngineView
//     snapshot that the engine republishes each cycle. Two small mutexes guard
//     the command queue and the published view; the render loop never blocks on
//     IPC.
// ============================================================================
#pragma once

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "execution/kdb_client.hpp"
#include "execution/matching.hpp"
#include "execution/oms.hpp"
#include "execution/portfolio.hpp"
#include "execution/risk.hpp"
#include "execution/types.hpp"

namespace el {

// Per-symbol row shown in the UI: live top-of-book + our position in it.
struct SymbolView {
    std::string symbol;
    bool has_quote = false;
    double bid = 0, ask = 0, mid = 0, bsize = 0, asize = 0;
    double net_qty = 0, avg_price = 0, unrealized = 0, realized = 0;
};

// Immutable snapshot the GUI renders. Copied out under a mutex each frame.
struct EngineView {
    bool connected = false;
    bool risk_killed = false;
    std::string status;
    double initial_capital = 0, cash = 0, equity = 0;
    double total_pnl = 0, realized = 0, unrealized = 0;
    std::vector<SymbolView> symbols;
    std::vector<std::string> log;       // most-recent last
    std::vector<std::string> products;  // catalog for the ticker search
    std::string focus;                  // symbol whose chart is populated
    std::vector<double> price_history;  // last-trade prices for `focus`
    std::vector<double> pnl_history;    // account PnL over time
    std::vector<double> equity_history; // account equity over time
};

// Command posted from the GUI to the engine thread.
struct Command {
    enum Type { Buy, Sell, Flatten, AddSymbol, Kill, SetFocus } type;
    std::string symbol;
    double amount = 0;          // notional ($) when use_notional, else quantity
    bool use_notional = true;
};

class TradingEngine {
public:
    // `port` is the RDB (quotes/history); `tp_port` is the tickerplant (control
    // plane: add-symbol + product catalog).
    TradingEngine(std::string host, int port, int tp_port,
                  std::vector<std::string> initial_symbols,
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

    std::string host_;
    int port_;
    int tp_port_;
    std::vector<std::string> symbols_;  // engine-thread owned
    double initial_capital_;

    KdbClient kdb_;    // RDB connection: quotes + history
    KdbClient ctrl_;   // tickerplant connection: control plane + catalog
    Portfolio pf_;
    RiskManager risk_;
    PaperMatchingEngine matcher_;
    OrderManager oms_;
    std::map<std::string, Quote> quotes_;

    std::string focus_;                      // symbol charted in the UI
    std::vector<std::string> products_;      // cached catalog
    std::vector<double> price_hist_;         // last-trade prices for focus_
    std::deque<double> pnl_hist_, eq_hist_;  // account time series (ring)
    int cycle_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex cmd_mu_;
    std::deque<Command> cmds_;

    std::mutex view_mu_;
    EngineView view_;
    std::deque<std::string> log_;
};

}  // namespace el
