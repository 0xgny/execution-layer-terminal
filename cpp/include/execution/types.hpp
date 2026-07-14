// ============================================================================
// types.hpp -- Core value types shared across the execution layer.
//
// These are deliberately plain structs/enums with no dependencies: the OMS,
// risk manager, matching engine, and strategies all speak in terms of them.
// They mirror the concepts the Python/KDB+ side already produces (a `Signal`
// is what the Analysis Engine will publish into the KDB+ `signal` table).
// ============================================================================
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace el {

using OrderId = std::uint64_t;
using TimestampNs = std::int64_t;

enum class Side { Buy, Sell };

inline const char* to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline double signed_qty(Side s, double qty) { return s == Side::Buy ? qty : -qty; }

// Which market a symbol belongs to. Crypto flows live via Coinbase -> KDB+;
// Stock flows via the Alpaca REST client (see alpaca_client.hpp/stock_feed.hpp).
// The OMS, risk manager, and matching engine are asset-agnostic -- they only
// ever see a Quote/Order/Fill, never this tag. It exists purely so the engine
// can route control-plane actions (AddSymbol) and the GUI can label rows.
enum class AssetClass { Crypto, Stock };

inline const char* to_string(AssetClass a) { return a == AssetClass::Crypto ? "CRYPTO" : "STOCK"; }

enum class OrderType { Market, Limit };
inline const char* to_string(OrderType t) { return t == OrderType::Market ? "MKT" : "LMT"; }

// Order lifecycle. Legal transitions are enforced in the OMS (see oms.cpp).
enum class OrderStatus { New, Sent, PartiallyFilled, Filled, Cancelled, Rejected };

inline const char* to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::New:             return "NEW";
        case OrderStatus::Sent:            return "SENT";
        case OrderStatus::PartiallyFilled: return "PARTIAL";
        case OrderStatus::Filled:          return "FILLED";
        case OrderStatus::Cancelled:       return "CANCELLED";
        case OrderStatus::Rejected:        return "REJECTED";
    }
    return "?";
}

// Top-of-book snapshot -- the execution layer's view of the market. Populated
// today by the mock source; later by the KDB+ RDB (quote table).
struct Quote {
    std::string symbol;
    double bid = 0.0;
    double ask = 0.0;
    double bsize = 0.0;
    double asize = 0.0;
    TimestampNs ts_ns = 0;
    AssetClass asset_class = AssetClass::Crypto;

    double mid() const { return 0.5 * (bid + ask); }
    double spread() const { return ask - bid; }
};

// A trade intention emitted by a Strategy (or, later, read from the KDB+
// `signal` table produced by the Python Analysis Engine).
struct Signal {
    std::string symbol;
    Side side = Side::Buy;
    double target_qty = 0.0;
    OrderType type = OrderType::Market;
    double limit_price = 0.0;   // used only for OrderType::Limit
    std::string strategy;       // provenance, e.g. "momentum"
    TimestampNs ts_ns = 0;
};

struct Order {
    OrderId id = 0;
    std::string symbol;
    Side side = Side::Buy;
    OrderType type = OrderType::Market;
    double qty = 0.0;
    double limit_price = 0.0;
    double filled_qty = 0.0;
    double avg_fill_price = 0.0;
    OrderStatus status = OrderStatus::New;
    std::string strategy;
    std::string reject_reason;  // set when status == Rejected
    TimestampNs ts_ns = 0;

    double leaves() const { return qty - filled_qty; }
};

struct Fill {
    OrderId order_id = 0;
    std::string symbol;
    Side side = Side::Buy;
    double qty = 0.0;
    double price = 0.0;
    TimestampNs ts_ns = 0;
};

// Signed net position with running average cost and realized PnL.
struct Position {
    std::string symbol;
    double net_qty = 0.0;       // >0 long, <0 short
    double avg_price = 0.0;     // average cost of the open position
    double realized_pnl = 0.0;

    // Apply a fill using standard signed-position accounting: extend the
    // position (re-average) or reduce/flip it (realize PnL on the closed part).
    void apply(const Fill& f) {
        const double sq = signed_qty(f.side, f.qty);
        const double new_net = net_qty + sq;

        const bool opening = (net_qty == 0.0) || ((net_qty > 0.0) == (sq > 0.0));
        if (opening) {
            // same direction (or from flat): weighted-average the cost
            avg_price = (avg_price * std::abs(net_qty) + f.price * std::abs(sq))
                        / std::abs(new_net);
        } else {
            // reducing or flipping: realize PnL on the closed quantity
            const double closed = std::min(std::abs(sq), std::abs(net_qty));
            const double dir = (net_qty > 0.0) ? 1.0 : -1.0;
            realized_pnl += (f.price - avg_price) * closed * dir;
            if (std::abs(sq) > std::abs(net_qty)) {
                avg_price = f.price;  // flipped: remainder opens at the fill price
            }
        }

        net_qty = new_net;
        if (net_qty == 0.0) avg_price = 0.0;
    }

    // Mark-to-market PnL of the open position at `mark`.
    double unrealized(double mark) const { return (mark - avg_price) * net_qty; }
};

}  // namespace el
