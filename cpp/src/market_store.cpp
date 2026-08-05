#include "execution/market_store.hpp"

#include <algorithm>

namespace el {

void MarketStore::on_quote(const Quote& q) {
    if (q.symbol.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    last_quote_[q.symbol] = q;
}

void MarketStore::on_trade(const std::string& symbol, double price) {
    if (symbol.empty() || !(price > 0.0)) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::deque<double>& h = history_[symbol];
    h.push_back(price);
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

std::vector<double> MarketStore::history(const std::string& symbol, int n) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = history_.find(symbol);
    if (it == history_.end() || n <= 0) return {};
    const std::deque<double>& h = it->second;
    const std::size_t take = std::min(static_cast<std::size_t>(n), h.size());
    return std::vector<double>(h.end() - static_cast<std::ptrdiff_t>(take), h.end());
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

}  // namespace el
