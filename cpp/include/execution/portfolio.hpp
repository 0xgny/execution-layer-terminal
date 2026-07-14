// ============================================================================
// portfolio.hpp -- Cash + positions + PnL accounting for one trading account.
//
// The user funds the account with an initial capital amount on launch. Each
// paper fill moves cash and updates the per-symbol Position (which tracks avg
// cost and realized PnL). Equity is cash + mark-to-market value of open
// positions; total PnL is equity - initial capital.
// ============================================================================
#pragma once

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "execution/types.hpp"

namespace el {

using MarkMap = std::map<std::string, double>;  // symbol -> mark price

// A fully closed round trip: the position went from nonzero back to exactly
// zero on this fill. Recorded so the GUI can split Positions into an open
// ("Current") table and a closed ("Previous") one -- today the raw Position
// map keeps every symbol forever (net_qty resets to 0 but the entry stays),
// so there was no way to tell "closed" from "never opened" without this.
struct ClosedPosition {
    std::string symbol;
    double qty = 0.0;          // size that was closed on this round trip
    double avg_entry = 0.0;
    double exit_price = 0.0;
    double realized_pnl = 0.0; // pnl of this round trip only, not lifetime
    TimestampNs closed_ts_ns = 0;
};

class Portfolio {
public:
    void fund(double capital) { initial_capital_ = capital; cash_ = capital; }

    double initial_capital() const { return initial_capital_; }
    double cash() const { return cash_; }
    const std::map<std::string, Position>& positions() const { return positions_; }
    const std::vector<ClosedPosition>& closed_positions() const { return closed_; }

    double net_qty(const std::string& sym) const {
        auto it = positions_.find(sym);
        return it == positions_.end() ? 0.0 : it->second.net_qty;
    }

    // Apply a fill: update the position and move cash (buy spends, sell receives).
    // If this fill closes the position exactly flat, record a ClosedPosition.
    void apply(const Fill& f) {
        Position& p = positions_[f.symbol];
        if (p.symbol.empty()) p.symbol = f.symbol;

        const double net_before = p.net_qty;
        const double avg_before = p.avg_price;
        const double realized_before = p.realized_pnl;

        p.apply(f);
        cash_ += (f.side == Side::Buy ? -1.0 : 1.0) * f.qty * f.price;

        if (net_before != 0.0 && p.net_qty == 0.0) {
            closed_.push_back(ClosedPosition{
                f.symbol, std::abs(net_before), avg_before, f.price,
                p.realized_pnl - realized_before, f.ts_ns});
        }
    }

    double positions_value(const MarkMap& marks) const {
        double v = 0.0;
        for (const auto& [s, p] : positions_) {
            auto it = marks.find(s);
            if (it != marks.end()) v += p.net_qty * it->second;
        }
        return v;
    }
    double realized() const {
        double r = 0.0;
        for (const auto& [s, p] : positions_) r += p.realized_pnl;
        return r;
    }
    double unrealized(const MarkMap& marks) const {
        double u = 0.0;
        for (const auto& [s, p] : positions_) {
            auto it = marks.find(s);
            if (it != marks.end()) u += p.unrealized(it->second);
        }
        return u;
    }
    double equity(const MarkMap& marks) const { return cash_ + positions_value(marks); }
    double total_pnl(const MarkMap& marks) const { return equity(marks) - initial_capital_; }

private:
    double initial_capital_ = 0.0;
    double cash_ = 0.0;
    std::map<std::string, Position> positions_;
    std::vector<ClosedPosition> closed_;
};

}  // namespace el
