#include "execution/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace el {

TradingEngine::TradingEngine(std::string host, int port, int tp_port,
                             std::vector<std::string> initial_symbols,
                             double initial_capital, RiskLimits limits)
    : host_(std::move(host)),
      port_(port),
      tp_port_(tp_port),
      symbols_(std::move(initial_symbols)),
      initial_capital_(initial_capital),
      risk_(limits),
      oms_(risk_, matcher_, pf_) {
    if (!symbols_.empty()) focus_ = symbols_.front();
}

TradingEngine::~TradingEngine() { stop(); }

void TradingEngine::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void TradingEngine::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
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
    pf_.fund(initial_capital_);
    if (!kdb_.connect(host_, port_)) {
        log("connect failed: " + kdb_.last_error());
    } else {
        log("connected to RDB " + host_ + ":" + std::to_string(port_));
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

        // 2. reconnect if needed
        if (!kdb_.connected()) {
            if (kdb_.connect(host_, port_)) log("reconnected to RDB");
        }

        // 3. poll live quotes
        if (kdb_.connected()) {
            auto qs = kdb_.snapshot(symbols_);
            for (auto& q : qs) quotes_[q.symbol] = q;
        }

        publish();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void TradingEngine::process(const Command& c) {
    if (c.type == Command::AddSymbol) {
        if (!c.symbol.empty() &&
            std::find(symbols_.begin(), symbols_.end(), c.symbol) == symbols_.end()) {
            symbols_.push_back(c.symbol);
            log("watching " + c.symbol);
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
    const double ref = side == Side::Buy ? q.ask : q.bid;
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
    v.connected = kdb_.connected();
    v.risk_killed = risk_.killed();
    v.status = v.connected ? "LIVE" : "DISCONNECTED";
    v.initial_capital = pf_.initial_capital();
    v.cash = pf_.cash();
    v.equity = pf_.equity(marks);
    v.realized = pf_.realized();
    v.unrealized = pf_.unrealized(marks);
    v.total_pnl = pf_.total_pnl(marks);

    for (const std::string& sym : symbols_) {
        SymbolView sv;
        sv.symbol = sym;
        auto qit = quotes_.find(sym);
        if (qit != quotes_.end()) {
            sv.has_quote = true;
            sv.bid = qit->second.bid;
            sv.ask = qit->second.ask;
            sv.mid = qit->second.mid();
            sv.bsize = qit->second.bsize;
            sv.asize = qit->second.asize;
        }
        const auto& positions = pf_.positions();
        auto pit = positions.find(sym);
        if (pit != positions.end()) {
            sv.net_qty = pit->second.net_qty;
            sv.avg_price = pit->second.avg_price;
            sv.realized = pit->second.realized_pnl;
            if (sv.has_quote) sv.unrealized = pit->second.unrealized(sv.mid);
        }
        v.symbols.push_back(std::move(sv));
    }
    v.log.assign(log_.begin(), log_.end());

    std::lock_guard<std::mutex> lk(view_mu_);
    view_ = std::move(v);
}

}  // namespace el
