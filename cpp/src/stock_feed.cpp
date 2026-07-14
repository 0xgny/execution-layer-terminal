#include "execution/stock_feed.hpp"

#include <algorithm>
#include <chrono>

namespace el {

namespace {
constexpr int kQuoteRefreshSecs = 10;
constexpr int kBarsRefreshSecs = 300;  // daily bars barely change intraday
constexpr int kBarsLookback = 120;

TimestampNs now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

StockFeed::~StockFeed() { stop(); }

void StockFeed::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void StockFeed::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void StockFeed::watch(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(sym_mu_);
    if (std::find(symbols_.begin(), symbols_.end(), symbol) == symbols_.end())
        symbols_.push_back(symbol);
}

std::map<std::string, Quote> StockFeed::snapshot() {
    std::lock_guard<std::mutex> lk(data_mu_);
    return quotes_;
}

std::vector<double> StockFeed::history(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(data_mu_);
    auto it = bars_.find(symbol);
    return it == bars_.end() ? std::vector<double>{} : it->second;
}

std::vector<std::string> StockFeed::errors() {
    std::lock_guard<std::mutex> lk(err_mu_);
    std::vector<std::string> out(errors_.begin(), errors_.end());
    errors_.clear();
    return out;
}

void StockFeed::note_error(const std::string& e) {
    std::lock_guard<std::mutex> lk(err_mu_);
    errors_.push_back(e);
    while (errors_.size() > 50) errors_.pop_front();
}

void StockFeed::run() {
    while (running_) {
        std::vector<std::string> syms;
        {
            std::lock_guard<std::mutex> lk(sym_mu_);
            syms = symbols_;
        }

        const TimestampNs t = now_ns();
        for (const std::string& sym : syms) {
            if (!running_) break;

            bool need_quote, need_bars;
            {
                std::lock_guard<std::mutex> lk(data_mu_);
                auto qit = last_quote_fetch_.find(sym);
                need_quote = (qit == last_quote_fetch_.end()) ||
                             (t - qit->second) > (TimestampNs)kQuoteRefreshSecs * 1'000'000'000LL;
                auto bit = last_bars_fetch_.find(sym);
                need_bars = (bit == last_bars_fetch_.end()) ||
                            (t - bit->second) > (TimestampNs)kBarsRefreshSecs * 1'000'000'000LL;
            }

            if (need_quote) {
                if (auto q = client_.latest_quote(sym)) {
                    std::lock_guard<std::mutex> lk(data_mu_);
                    quotes_[sym] = *q;
                    last_quote_fetch_[sym] = now_ns();
                } else {
                    note_error("alpaca quote " + sym + ": " + client_.last_error());
                    std::lock_guard<std::mutex> lk(data_mu_);
                    last_quote_fetch_[sym] = now_ns();  // back off; don't hammer on failure
                }
            }

            if (need_bars) {
                auto closes = client_.daily_bars(sym, kBarsLookback);
                std::lock_guard<std::mutex> lk(data_mu_);
                if (!closes.empty()) bars_[sym] = std::move(closes);
                else note_error("alpaca bars " + sym + ": " + client_.last_error());
                last_bars_fetch_[sym] = now_ns();
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace el
