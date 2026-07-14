// ============================================================================
// oms.hpp -- Order Management System: the heart of the execution layer.
//
// Turns a Signal (from a strategy, the GUI order ticket, or later the KDB+
// `signal` table) into an Order, gates it, routes it to the matching engine,
// and applies fills to the Portfolio. Enforces a legal order state machine.
//
// Two gates run before an order can fill:
//   1. business rules that depend on account state (buying power for buys;
//      no shorting for sells -- this is long-only spot),
//   2. RiskManager limits (max order/position size, notional) + kill switch.
// ============================================================================
#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "execution/matching.hpp"
#include "execution/portfolio.hpp"
#include "execution/risk.hpp"
#include "execution/types.hpp"

namespace el {

class OrderManager {
public:
    OrderManager(RiskManager& risk, PaperMatchingEngine& matcher, Portfolio& portfolio)
        : risk_(risk), matcher_(matcher), portfolio_(portfolio) {}

    // Submit a signal. Returns the created order's id (even if rejected, so the
    // caller can inspect the reason), or nullopt for a degenerate signal.
    std::optional<OrderId> submit(const Signal& sig, const Quote& market);

    // Convenience: fully flatten a symbol at the current market (a market sell
    // of the entire long position). No-op if flat. Returns the order id.
    std::optional<OrderId> flatten(const std::string& symbol, const Quote& market);

    void on_fill(const Fill& f);

    const std::unordered_map<OrderId, Order>& orders() const { return orders_; }
    const std::vector<Fill>& fills() const { return fills_; }
    const Portfolio& portfolio() const { return portfolio_; }

private:
    bool transition(Order& o, OrderStatus to);
    double reference_price(const Order& o, const Quote& market) const;

    RiskManager& risk_;
    PaperMatchingEngine& matcher_;
    Portfolio& portfolio_;

    OrderId next_id_ = 1;
    std::unordered_map<OrderId, Order> orders_;
    std::vector<Fill> fills_;
};

}  // namespace el
