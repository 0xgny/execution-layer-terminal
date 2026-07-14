#include "execution/risk.hpp"

#include <cmath>

namespace el {

void RiskManager::kill(std::string reason) {
    killed_ = true;
    kill_reason_ = std::move(reason);
}

RiskDecision RiskManager::check(const Order& order, const Position& current,
                                const Quote& market) const {
    if (killed_) {
        return RiskDecision::deny("kill switch active: " + kill_reason_);
    }
    if (!(order.qty > 0.0)) {
        return RiskDecision::deny("non-positive order qty");
    }
    if (order.qty > limits_.max_order_qty) {
        return RiskDecision::deny("order qty exceeds max_order_qty");
    }

    // Reference price for notional: the limit price for limit orders, else the
    // side we'd cross (buy -> ask, sell -> bid), falling back to mid.
    double ref = order.type == OrderType::Limit ? order.limit_price
               : order.side == Side::Buy        ? market.ask
                                                : market.bid;
    if (!(ref > 0.0)) ref = market.mid();

    const double notional = order.qty * ref;
    if (notional > limits_.max_order_notional) {
        return RiskDecision::deny("order notional exceeds max_order_notional");
    }

    // Projected position if this order fully fills.
    const double projected = current.net_qty + signed_qty(order.side, order.qty);
    if (std::abs(projected) > limits_.max_position_qty) {
        return RiskDecision::deny("projected position exceeds max_position_qty");
    }

    return RiskDecision::ok();
}

}  // namespace el
