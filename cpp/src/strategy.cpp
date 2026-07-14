#include "execution/strategy.hpp"

namespace el {

double MovingAverageCross::sma(const std::deque<double>& w, int n, double /*sum_all*/) const {
    const int have = static_cast<int>(w.size());
    const int take = n < have ? n : have;
    if (take == 0) return 0.0;
    double s = 0.0;
    for (int i = have - take; i < have; ++i) s += w[static_cast<std::size_t>(i)];
    return s / take;
}

std::optional<Signal> MovingAverageCross::on_quote(const Quote& q) {
    SymState& st = state_[q.symbol];
    st.window.push_back(q.mid());
    if (static_cast<int>(st.window.size()) > slow_) st.window.pop_front();

    // Need a full slow window before trading.
    if (static_cast<int>(st.window.size()) < slow_) return std::nullopt;

    const double fast_ma = sma(st.window, fast_, 0.0);
    const double slow_ma = sma(st.window, slow_, 0.0);
    const int sign = fast_ma > slow_ma ? 1 : (fast_ma < slow_ma ? -1 : 0);

    std::optional<Signal> out;

    // Cross up: go long if we aren't already.
    if (st.prior_sign <= 0 && sign > 0 && !st.is_long) {
        st.is_long = true;
        out = Signal{q.symbol, Side::Buy, order_qty_, OrderType::Market,
                     0.0, name(), q.ts_ns};
    }
    // Cross down: flatten if we're long.
    else if (st.prior_sign >= 0 && sign < 0 && st.is_long) {
        st.is_long = false;
        out = Signal{q.symbol, Side::Sell, order_qty_, OrderType::Market,
                     0.0, name(), q.ts_ns};
    }

    st.prior_sign = sign;
    return out;
}

}  // namespace el
