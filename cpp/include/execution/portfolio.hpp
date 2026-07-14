// ============================================================================
// portfolio.hpp -- Cash + positions + PnL accounting for one trading account.
//
// The user funds the account with an initial capital amount on launch. Each
// paper fill moves cash and updates the per-symbol Position (which tracks avg
// cost and realized PnL). Equity is cash + mark-to-market value of open
// positions; total PnL is equity - initial capital.
// ============================================================================
#pragma once

#include <map>
#include <string>

#include "execution/types.hpp"

namespace el {

using MarkMap = std::map<std::string, double>;  // symbol -> mark price

class Portfolio {
public:
    void fund(double capital) { initial_capital_ = capital; cash_ = capital; }

    double initial_capital() const { return initial_capital_; }
    double cash() const { return cash_; }
    const std::map<std::string, Position>& positions() const { return positions_; }

    double net_qty(const std::string& sym) const {
        auto it = positions_.find(sym);
        return it == positions_.end() ? 0.0 : it->second.net_qty;
    }

    // Apply a fill: update the position and move cash (buy spends, sell receives).
    void apply(const Fill& f) {
        Position& p = positions_[f.symbol];
        if (p.symbol.empty()) p.symbol = f.symbol;
        p.apply(f);
        cash_ += (f.side == Side::Buy ? -1.0 : 1.0) * f.qty * f.price;
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
};

}  // namespace el
