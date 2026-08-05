#include "execution/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace el {

TradingEngine::TradingEngine(int feed_port, std::vector<std::string> initial_symbols,
                             double initial_capital, RiskLimits limits)
    : symbols_(std::move(initial_symbols)),
      initial_capital_(initial_capital),
      feed_(store_, feed_port),
      risk_(limits),
      oms_(risk_, matcher_, pf_) {
    if (!symbols_.empty()) focus_ = symbols_.front();
}

TradingEngine::~TradingEngine() { stop(); }

void TradingEngine::start() {
    if (running_) return;
    running_ = true;
    stocks_.start();
    thread_ = std::thread([this] { run(); });
}

void TradingEngine::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    feed_proc_.stop();  // before feed_, so the child's socket closes first
    feed_.stop();
    stocks_.stop();
}

void TradingEngine::post(const Command& c) {
    std::lock_guard<std::mutex> lk(cmd_mu_);
    cmds_.push_back(c);
}

EngineView TradingEngine::view() {
    std::lock_guard<std::mutex> lk(view_mu_);
    return view_;
}

void TradingEngine::log(const std::string& s) {
    log_.push_back(s);
    while (log_.size() > 200) log_.pop_front();
}

void TradingEngine::run() {
    using clock = std::chrono::steady_clock;
    constexpr auto kHistoryEvery = std::chrono::milliseconds(450);
    constexpr auto kCatalogEvery = std::chrono::seconds(2);

    pf_.fund(initial_capital_);
    if (!feed_.start()) {
        log("feed socket failed: " + feed_.last_error());
    } else {
        log("listening for feedhandler on 127.0.0.1:" + std::to_string(feed_.port()));
        // Packaged builds start their own feedhandler; dev trees don't have one
        // bundled and expect scripts/run.sh, so "not bundled" is silent.
        if (feed_proc_.start(feed_.port())) log("started bundled feedhandler");
        else if (feed_proc_.last_error() != "not bundled")
            log("feedhandler launch failed: " + feed_proc_.last_error());
    }

    while (running_) {
        // 1. drain commands
        {
            std::deque<Command> pending;
            {
                std::lock_guard<std::mutex> lk(cmd_mu_);
                pending.swap(cmds_);
            }
            for (const Command& c : pending) process(c);
        }

        // 2. read live quotes. Both sources are local, thread-safe caches kept
        //    current by their own threads (FeedServer for crypto, StockFeed for
        //    stocks), so this is two mutexed map copies -- no I/O on this thread.
        for (auto& q : store_.snapshot(symbols_)) quotes_[q.symbol] = q;
        for (auto& [sym, q] : stocks_.snapshot()) quotes_[sym] = q;

        // 3. periodic: price history for the focused symbol, and the product
        //    catalog. Cheaper than doing them every cycle.
        const auto now = clock::now();
        if (!focus_.empty() && now >= next_history_) {
            price_hist_ = is_stock(focus_) ? stocks_.history(focus_)
                                           : store_.history(focus_, 300);
            next_history_ = now + kHistoryEvery;
        }
        for (const auto& e : stocks_.errors()) log(e);
        if (now >= next_catalog_ || products_.empty()) {
            products_ = store_.products();
            // Auto-populate the watch list from whatever the feed is streaming.
            for (const auto& s : store_.universe())
                if (std::find(symbols_.begin(), symbols_.end(), s) == symbols_.end())
                    symbols_.push_back(s);
            if (focus_.empty() && !symbols_.empty()) focus_ = symbols_.front();
            next_catalog_ = now + kCatalogEvery;
        }

        publish();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void TradingEngine::process(const Command& c) {
    if (c.type == Command::AddSymbol) {
        if (c.symbol.empty()) return;
        if (c.asset_class == AssetClass::Stock) {
            if (std::find(stock_symbols_.begin(), stock_symbols_.end(), c.symbol) == stock_symbols_.end()) {
                stock_symbols_.push_back(c.symbol);
                stocks_.watch(c.symbol);  // starts polling on StockFeed's thread
                focus_ = c.symbol;
                price_hist_.clear();
                log("watching " + c.symbol + " (stock, Alpaca/IEX)");
            }
        } else if (std::find(symbols_.begin(), symbols_.end(), c.symbol) == symbols_.end()) {
            symbols_.push_back(c.symbol);
            store_.add_symbol(c.symbol);  // ask the feed to start streaming it
            focus_ = c.symbol;           // focus the chart on the new ticker
            price_hist_.clear();
            log("watching " + c.symbol);
        }
        return;
    }
    if (c.type == Command::SetFocus) {
        if (!c.symbol.empty() && c.symbol != focus_) {
            focus_ = c.symbol;
            price_hist_.clear();
        }
        return;
    }
    if (c.type == Command::Kill) {
        risk_.kill("manual halt (terminal)");
        log("KILL SWITCH engaged");
        return;
    }

    auto it = quotes_.find(c.symbol);
    if (it == quotes_.end()) { log("no quote yet for " + c.symbol); return; }
    const Quote& q = it->second;

    if (c.type == Command::Flatten) {
        if (auto id = oms_.flatten(c.symbol, q)) {
            const Order& o = oms_.orders().at(*id);
            log("FLATTEN " + c.symbol + " -> " + to_string(o.status));
        }
        return;
    }

    const Side side = c.type == Command::Buy ? Side::Buy : Side::Sell;
    const double ref = q.touch(side);
    double qty = c.use_notional ? (ref > 0 ? c.amount / ref : 0.0) : c.amount;
    if (!(qty > 0.0)) { log("bad order qty for " + c.symbol); return; }

    Signal s{c.symbol, side, qty, OrderType::Market, 0.0, "manual", q.ts_ns};
    if (auto id = oms_.submit(s, q)) {
        const Order& o = oms_.orders().at(*id);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s %.6f %s @ %.2f -> %s%s%s",
                      to_string(side), qty, c.symbol.c_str(), ref, to_string(o.status),
                      o.status == OrderStatus::Rejected ? " (" : "",
                      o.status == OrderStatus::Rejected ? o.reject_reason.c_str() : "");
        std::string line = buf;
        if (o.status == OrderStatus::Rejected) line += ")";
        log(line);
    }
}

void TradingEngine::publish() {
    MarkMap marks;
    for (const auto& [s, q] : quotes_) marks[s] = q.mid();

    EngineView v;
    v.connected = store_.connected();
    v.risk_killed = risk_.killed();
    v.status = v.connected ? "LIVE" : "DISCONNECTED";
    v.initial_capital = pf_.initial_capital();
    v.cash = pf_.cash();
    v.equity = pf_.equity(marks);
    v.realized = pf_.realized();
    v.unrealized = pf_.unrealized(marks);
    v.total_pnl = pf_.total_pnl(marks);

    // Sample the account time series (ring buffer, ~54s at 33 Hz).
    pnl_hist_.push_back(v.total_pnl);
    eq_hist_.push_back(v.equity);
    while (pnl_hist_.size() > 1800) pnl_hist_.pop_front();
    while (eq_hist_.size() > 1800) eq_hist_.pop_front();

    const auto& positions = pf_.positions();
    auto build_row = [&](const std::string& sym, AssetClass cls) {
        SymbolView sv;
        sv.symbol = sym;
        sv.asset_class = cls;
        auto qit = quotes_.find(sym);
        if (qit != quotes_.end()) {
            sv.has_quote = true;
            sv.bid = qit->second.bid;
            sv.ask = qit->second.ask;
            sv.mid = qit->second.mid();
            sv.bsize = qit->second.bsize;
            sv.asize = qit->second.asize;
        }
        auto pit = positions.find(sym);
        if (pit != positions.end()) {
            sv.net_qty = pit->second.net_qty;
            sv.avg_price = pit->second.avg_price;
            sv.realized = pit->second.realized_pnl;
            if (sv.has_quote) sv.unrealized = pit->second.unrealized(sv.mid);
        }
        return sv;
    };
    for (const std::string& sym : symbols_) v.symbols.push_back(build_row(sym, AssetClass::Crypto));
    for (const std::string& sym : stock_symbols_) v.symbols.push_back(build_row(sym, AssetClass::Stock));

    for (auto it = pf_.closed_positions().rbegin(); it != pf_.closed_positions().rend(); ++it) {
        ClosedPositionView cv;
        cv.symbol = it->symbol;
        cv.asset_class = is_stock(it->symbol) ? AssetClass::Stock : AssetClass::Crypto;
        cv.qty = it->qty;
        cv.avg_entry = it->avg_entry;
        cv.exit_price = it->exit_price;
        cv.realized_pnl = it->realized_pnl;
        cv.closed_ts_ns = it->closed_ts_ns;
        v.closed_positions.push_back(cv);
    }

    v.log.assign(log_.begin(), log_.end());
    v.products = products_;
    v.focus = focus_;
    v.price_history = price_hist_;
    v.pnl_history.assign(pnl_hist_.begin(), pnl_hist_.end());
    v.equity_history.assign(eq_hist_.begin(), eq_hist_.end());
    v.stocks_enabled = stocks_.configured();

    std::lock_guard<std::mutex> lk(view_mu_);
    view_ = std::move(v);
}

bool TradingEngine::is_stock(const std::string& symbol) const {
    return std::find(stock_symbols_.begin(), stock_symbols_.end(), symbol) != stock_symbols_.end();
}

}  // namespace el
