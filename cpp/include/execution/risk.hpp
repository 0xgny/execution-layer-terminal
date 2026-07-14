// ============================================================================
// risk.hpp -- Pre-trade risk checks + kill switch.
//
// Every order passes through RiskManager::check() BEFORE it can reach the
// matching engine / an exchange. This is the line between "an execution layer"
// and "a script that fires orders". Keep it simple, explicit, and fail-closed.
// ============================================================================
#pragma once

#include <string>

#include "execution/types.hpp"

namespace el {

struct RiskLimits {
    double max_order_qty = 1.0;          // largest single order (base units)
    double max_position_qty = 5.0;       // largest |net position| per symbol
    double max_order_notional = 1e6;     // largest single order in quote ccy
};

struct RiskDecision {
    bool allowed = false;
    std::string reason;   // human-readable; recorded on rejected orders

    static RiskDecision ok() { return {true, ""}; }
    static RiskDecision deny(std::string why) { return {false, std::move(why)}; }
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits) : limits_(limits) {}

    // Check a proposed order against limits, given the current position in that
    // symbol and the prevailing market (for notional). Fail-closed.
    RiskDecision check(const Order& order, const Position& current,
                       const Quote& market) const;

    // Kill switch: once tripped, every subsequent check() is denied.
    void kill(std::string reason);
    bool killed() const { return killed_; }
    const std::string& kill_reason() const { return kill_reason_; }

    const RiskLimits& limits() const { return limits_; }

private:
    RiskLimits limits_;
    bool killed_ = false;
    std::string kill_reason_;
};

}  // namespace el
