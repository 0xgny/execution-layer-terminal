#include "execution/oms.hpp"

#include <iostream>

namespace el {

namespace {
bool legal(OrderStatus from, OrderStatus to) {
    switch (from) {
        case OrderStatus::New:
            return to == OrderStatus::Sent || to == OrderStatus::Rejected;
        case OrderStatus::Sent:
            return to == OrderStatus::PartiallyFilled || to == OrderStatus::Filled
                || to == OrderStatus::Cancelled || to == OrderStatus::Rejected;
        case OrderStatus::PartiallyFilled:
            return to == OrderStatus::PartiallyFilled || to == OrderStatus::Filled
                || to == OrderStatus::Cancelled;
        default:  // Filled / Cancelled / Rejected are terminal
            return false;
    }
}
}  // namespace

bool OrderManager::transition(Order& o, OrderStatus to) {
    if (!legal(o.status, to)) {
        std::cerr << "[oms] illegal transition " << to_string(o.status) << " -> "
                  << to_string(to) << " on order " << o.id << "\n";
        return false;
    }
    o.status = to;
    return true;
}

double OrderManager::reference_price(const Order& o, const Quote& market) const {
    if (o.type == OrderType::Limit && o.limit_price > 0.0) return o.limit_price;
    double ref = o.side == Side::Buy ? market.ask : market.bid;
    return ref > 0.0 ? ref : market.mid();
}

std::optional<OrderId> OrderManager::submit(const Signal& sig, const Quote& market) {
    if (!(sig.target_qty > 0.0) || sig.symbol.empty()) return std::nullopt;

    Order o;
    o.id = next_id_++;
    o.symbol = sig.symbol;
    o.side = sig.side;
    o.type = sig.type;
    o.qty = sig.target_qty;
    o.limit_price = sig.limit_price;
    o.strategy = sig.strategy;
    o.ts_ns = sig.ts_ns;
    o.status = OrderStatus::New;

    const double ref = reference_price(o, market);

    // --- gate 1: account business rules (long-only spot) ---------------------
    std::string biz_reject;
    if (o.side == Side::Buy) {
        if (portfolio_.cash() < o.qty * ref)
            biz_reject = "insufficient buying power";
    } else {  // Sell
        if (portfolio_.net_qty(o.symbol) < o.qty - 1e-12)
            biz_reject = "insufficient position (no shorting)";
    }
    if (!biz_reject.empty()) {
        o.reject_reason = biz_reject;
        transition(o, OrderStatus::Rejected);
        orders_[o.id] = o;
        return o.id;
    }

    // --- gate 2: risk limits + kill switch -----------------------------------
    const Position dummy_pos{o.symbol, portfolio_.net_qty(o.symbol), 0.0, 0.0};
    const RiskDecision rd = risk_.check(o, dummy_pos, market);
    if (!rd.allowed) {
        o.reject_reason = rd.reason;
        transition(o, OrderStatus::Rejected);
        orders_[o.id] = o;
        return o.id;
    }

    // --- route to the (paper) matching engine --------------------------------
    transition(o, OrderStatus::Sent);
    orders_[o.id] = o;  // store before fills so on_fill can find it
    for (const Fill& f : matcher_.fill(o, market)) on_fill(f);
    return o.id;
}

std::optional<OrderId> OrderManager::flatten(const std::string& symbol, const Quote& market) {
    const double net = portfolio_.net_qty(symbol);
    if (net <= 1e-12) return std::nullopt;  // nothing long to sell
    Signal s;
    s.symbol = symbol;
    s.side = Side::Sell;
    s.target_qty = net;
    s.type = OrderType::Market;
    s.strategy = "flatten";
    s.ts_ns = market.ts_ns;
    return submit(s, market);
}

void OrderManager::on_fill(const Fill& f) {
    auto it = orders_.find(f.order_id);
    if (it == orders_.end()) {
        std::cerr << "[oms] fill for unknown order " << f.order_id << "\n";
        return;
    }
    Order& o = it->second;

    const double new_filled = o.filled_qty + f.qty;
    if (new_filled > 0.0)
        o.avg_fill_price = (o.avg_fill_price * o.filled_qty + f.price * f.qty) / new_filled;
    o.filled_qty = new_filled;
    transition(o, o.leaves() <= 1e-12 ? OrderStatus::Filled : OrderStatus::PartiallyFilled);

    portfolio_.apply(f);
    fills_.push_back(f);
}

}  // namespace el
