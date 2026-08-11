#include "execution/stock_feed.hpp"

#include <algorithm>
#include <chrono>

namespace el {

namespace {
// One multi-symbol request per tick, so this is 60 req/min flat -- well inside
// Alpaca's 200/min free-plan budget regardless of watch-list size.
constexpr auto kQuoteInterval = std::chrono::milliseconds(1000);
// After a failed poll, wait this long before the next one. A failure is usually
// auth, rate limiting, or a dead link -- all of which get worse if we retry at
// full speed.
constexpr auto kErrorBackoff = std::chrono::milliseconds(5000);
// Granularity of the sleep, so stop() is honoured promptly rather than at the
// end of a full interval.
constexpr auto kTick = std::chrono::milliseconds(50);

// Price to chart for a quote. Prefers the mid, but a single-venue feed like
// IEX can legitimately show one side empty (see Quote::touch), and averaging in
// a zero would draw a cliff into the series.
double chart_price(const Quote& q) {
    if (q.bid > 0.0 && q.ask > 0.0) return q.mid();
    if (q.bid > 0.0) return q.bid;
    return q.ask;
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

std::vector<PricePoint> StockFeed::history(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(data_mu_);
    auto it = history_.find(symbol);
    if (it == history_.end()) return {};
    return std::vector<PricePoint>(it->second.begin(), it->second.end());
}

std::string StockFeed::name(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(data_mu_);
    auto it = names_.find(symbol);
    return it == names_.end() ? std::string{} : it->second;
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
    using clock = std::chrono::steady_clock;
    auto next_poll = clock::now();  // poll immediately

    // Advance the schedule from the deadline, not from "now". Adding the
    // interval to the *finish* time would fold each round-trip's latency into
    // the period, so the samples would drift apart -- and the chart's x-axis is
    // sample index, which only reads as time if the spacing is uniform.
    // std::max keeps a slow or backed-off poll from queueing up a burst.
    auto schedule_next = [&next_poll](std::chrono::milliseconds interval) {
        next_poll = std::max(next_poll + interval, clock::now());
    };

    while (running_) {
        if (clock::now() < next_poll) {
            std::this_thread::sleep_for(kTick);
            continue;
        }

        std::vector<std::string> syms;
        {
            std::lock_guard<std::mutex> lk(sym_mu_);
            syms = symbols_;
        }
        if (syms.empty()) {
            schedule_next(kQuoteInterval);
            continue;
        }

        // Per-symbol one-time work: the intraday backfill so the chart is
        // readable the instant a ticker is added, and the display name. Both
        // are cached even when they come back empty, so a symbol with no bars
        // (or no asset record) doesn't re-request forever.
        for (const std::string& sym : syms) {
            if (!running_) return;

            bool need_backfill, need_name;
            {
                std::lock_guard<std::mutex> lk(data_mu_);
                need_backfill = !backfilled_[sym];
                need_name = names_.find(sym) == names_.end();
            }

            if (need_backfill) {
                auto bars = client_.recent_minute_bars(sym, kBackfillMinutes);
                if (bars.empty()) note_error("alpaca backfill " + sym + ": " + client_.last_error());
                std::lock_guard<std::mutex> lk(data_mu_);
                backfilled_[sym] = true;
                // Prepend: a poll may already have appended live samples while
                // this request was in flight, and those are newer than any bar.
                std::deque<PricePoint>& h = history_[sym];
                h.insert(h.begin(), bars.begin(), bars.end());
                while (h.size() > kHistoryCap) h.pop_front();
            }

            if (need_name) {
                std::string nm = client_.asset_name(sym);
                std::lock_guard<std::mutex> lk(data_mu_);
                names_[sym] = nm;  // cache even an empty result; don't retry forever
            }
        }
        if (!running_) return;

        auto fresh = client_.latest_quotes(syms);
        if (fresh.empty()) {
            note_error("alpaca quotes: " + client_.last_error());
            schedule_next(kErrorBackoff);
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(data_mu_);
            for (auto& [sym, q] : fresh) {
                quotes_[sym] = q;
                // Sample the series once per poll. Appending unconditionally --
                // even when the price is unchanged -- is the point: the chart's
                // x-axis is elapsed polls, so a flat market has to draw a flat
                // line rather than simply stop advancing.
                const double px = chart_price(q);
                if (px > 0.0) {
                    std::deque<PricePoint>& h = history_[sym];
                    h.push_back({q.ts_ns, px});
                    while (h.size() > kHistoryCap) h.pop_front();
                }
            }
        }

        schedule_next(kQuoteInterval);
    }
}

}  // namespace el
