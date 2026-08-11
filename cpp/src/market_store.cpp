#include "execution/market_store.hpp"

#include <algorithm>
#include <limits>

namespace el {

void MarketStore::on_quote(const Quote& q) {
    if (q.symbol.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    last_quote_[q.symbol] = q;
}

void MarketStore::on_trade(const std::string& symbol, double price, TimestampNs ts_ns) {
    if (symbol.empty() || !(price > 0.0)) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::deque<PricePoint>& h = history_[symbol];
    h.push_back({ts_ns, price});
    while (h.size() > kHistoryCap) h.pop_front();
}

void MarketStore::backfill_history(const std::string& symbol,
                                   const std::vector<PricePoint>& points) {
    if (symbol.empty() || points.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::deque<PricePoint>& h = history_[symbol];
    const TimestampNs oldest =
        h.empty() ? std::numeric_limits<TimestampNs>::max() : h.front().ts_ns;

    std::vector<PricePoint> older;
    older.reserve(points.size());
    for (const PricePoint& p : points)
        if (p.price > 0.0 && p.ts_ns > 0 && p.ts_ns < oldest) older.push_back(p);

    h.insert(h.begin(), older.begin(), older.end());
    // Trimming from the front discards backfill before live tape, which is the
    // right precedence when the two together overflow the cap.
    while (h.size() > kHistoryCap) h.pop_front();
}

void MarketStore::set_products(std::vector<std::string> products) {
    std::lock_guard<std::mutex> lk(mu_);
    products_ = std::move(products);
}

void MarketStore::set_universe(std::vector<std::string> universe) {
    std::lock_guard<std::mutex> lk(mu_);
    universe_ = std::move(universe);
}

void MarketStore::set_names(std::map<std::string, std::string> names) {
    std::lock_guard<std::mutex> lk(mu_);
    names_ = std::move(names);
}

std::string MarketStore::name(const std::string& symbol) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = names_.find(symbol);
    return it == names_.end() ? std::string{} : it->second;
}

std::vector<std::string> MarketStore::take_requested() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.swap(requested_);
    return out;
}

std::vector<std::string> MarketStore::take_history_requests() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.swap(history_requests_);
    return out;
}

std::vector<Quote> MarketStore::snapshot(const std::vector<std::string>& symbols) const {
    std::vector<Quote> out;
    out.reserve(symbols.size());
    std::lock_guard<std::mutex> lk(mu_);
    for (const std::string& s : symbols) {
        auto it = last_quote_.find(s);
        if (it != last_quote_.end()) out.push_back(it->second);
    }
    return out;
}

std::vector<PricePoint> MarketStore::history(const std::string& symbol, int n) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = history_.find(symbol);
    if (it == history_.end() || n <= 0) return {};
    const std::deque<PricePoint>& h = it->second;
    const std::size_t take = std::min(static_cast<std::size_t>(n), h.size());
    return std::vector<PricePoint>(h.end() - static_cast<std::ptrdiff_t>(take), h.end());
}

std::vector<std::string> MarketStore::products() const {
    std::lock_guard<std::mutex> lk(mu_);
    return products_;
}

std::vector<std::string> MarketStore::universe() const {
    std::lock_guard<std::mutex> lk(mu_);
    return universe_;
}

void MarketStore::add_symbol(const std::string& symbol) {
    if (symbol.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (std::find(requested_.begin(), requested_.end(), symbol) == requested_.end())
        requested_.push_back(symbol);
}

void MarketStore::request_history(const std::string& symbol) {
    if (symbol.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (std::find(history_requests_.begin(), history_requests_.end(), symbol) ==
        history_requests_.end())
        history_requests_.push_back(symbol);
}

}  // namespace el
