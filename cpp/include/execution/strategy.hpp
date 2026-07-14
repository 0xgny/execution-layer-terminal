// ============================================================================
// strategy.hpp -- Strategy interface + a sample moving-average-cross strategy.
//
// A Strategy turns market data into Signals. This is the seam where user-defined
// strategies plug in. Today a strategy is a compiled C++ class; the roadmap adds
// declarative specs (YAML/JSON) and embedded scripting so users express
// strategies without recompiling. The sample here (fast/slow MA cross) is the
// same momentum idea prototyped in q-sim's analytics, kept intentionally small.
// ============================================================================
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>

#include "execution/types.hpp"

namespace el {

class Strategy {
public:
    virtual ~Strategy() = default;
    virtual std::optional<Signal> on_quote(const Quote& q) = 0;
    virtual std::string name() const = 0;
};

// Buy when the fast MA crosses above the slow MA (go/stay long); sell to flatten
// when it crosses below. Per-symbol state; emits a signal only on a cross, and
// only when it would change our intended flat/long stance (no order spamming).
class MovingAverageCross : public Strategy {
public:
    MovingAverageCross(int fast, int slow, double order_qty)
        : fast_(fast), slow_(slow), order_qty_(order_qty) {}

    std::optional<Signal> on_quote(const Quote& q) override;
    std::string name() const override { return "ma_cross"; }

private:
    struct SymState {
        std::deque<double> window;   // recent mids, up to slow_
        double fast_sum = 0.0;
        double slow_sum = 0.0;
        int prior_sign = 0;          // sign(fast-slow) last tick: -1/0/+1
        bool is_long = false;        // our current intended stance
    };

    double sma(const std::deque<double>& w, int n, double sum_all) const;

    int fast_;
    int slow_;
    double order_qty_;
    std::unordered_map<std::string, SymState> state_;
};

}  // namespace el
