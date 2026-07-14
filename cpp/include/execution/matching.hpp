// ============================================================================
// matching.hpp -- Paper (simulated) matching engine.
//
// Fills orders against the current top-of-book instead of routing to a real
// venue. This is the safety net and the backtester: no real money moves. A live
// exchange router would implement the same fill() contract later.
//
// Simplifications (documented on purpose):
//   * Fills happen at the touch (best bid/ask); no depth walking or slippage.
//   * Market orders fill in full immediately.
//   * Marketable limit orders fill at the touch; non-marketable limits rest
//     (return no fills) -- we don't model a resting book yet.
// ============================================================================
#pragma once

#include <vector>

#include "execution/types.hpp"

namespace el {

class PaperMatchingEngine {
public:
    // Attempt to fill `order` against `market`. Returns zero or more fills.
    std::vector<Fill> fill(const Order& order, const Quote& market);

private:
    TimestampNs seq_ = 0;
};

}  // namespace el
