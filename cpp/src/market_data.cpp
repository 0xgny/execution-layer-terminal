#include "execution/market_data.hpp"

#include <chrono>
#include <cmath>

namespace el {

MockMarketData::MockMarketData(std::vector<SymSpec> specs, int max_ticks, unsigned seed)
    : specs_(std::move(specs)), max_ticks_(max_ticks), rng_(seed) {
    mids_.reserve(specs_.size());
    for (const auto& s : specs_) mids_.push_back(s.start_mid);
}

bool MockMarketData::next(Quote& out) {
    if (emitted_ >= max_ticks_) return false;

    // Round-robin across symbols so multi-symbol runs interleave.
    const std::size_t i = static_cast<std::size_t>(emitted_) % specs_.size();
    const SymSpec& s = specs_[i];

    // Lognormal random-walk step on the mid.
    mids_[i] *= std::exp(s.vol * norm_(rng_));
    const double mid = mids_[i];
    const double half = 0.5 * s.spread;

    out.symbol = s.symbol;
    out.bid = mid - half;
    out.ask = mid + half;
    out.bsize = 1.0 + std::abs(norm_(rng_));
    out.asize = 1.0 + std::abs(norm_(rng_));
    out.ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();

    ++emitted_;
    return true;
}

std::string MockMarketData::describe() const {
    std::string d = "MockMarketData(";
    for (std::size_t i = 0; i < specs_.size(); ++i) {
        d += specs_[i].symbol;
        if (i + 1 < specs_.size()) d += ",";
    }
    d += ")";
    return d;
}

}  // namespace el
