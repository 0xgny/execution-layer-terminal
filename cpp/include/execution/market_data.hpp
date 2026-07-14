// ============================================================================
// market_data.hpp -- Source-agnostic market data feed for the execution layer.
//
// Mirrors the Python feedhandler's philosophy: the execution core depends only
// on this interface, so the *source* is swappable. Today: MockMarketData (a
// self-contained random walk, no dependencies). Next: KdbMarketData, which will
// subscribe to the RDB's `quote` table over the KDB+ C API (see the sketch in
// docs and cpp/README.md). Swapping sources touches nothing downstream.
// ============================================================================
#pragma once

#include <random>
#include <string>
#include <vector>

#include "execution/types.hpp"

namespace el {

class MarketDataSource {
public:
    virtual ~MarketDataSource() = default;

    // Fetch the next quote. Returns false when the source is exhausted (the mock
    // stops after a fixed number of ticks; a live source blocks/streams).
    virtual bool next(Quote& out) = 0;

    virtual std::string describe() const = 0;
};

// A deterministic-seeded random walk over one or more symbols, for development
// and for demonstrating the OMS/risk/matching without any network or KDB+.
class MockMarketData : public MarketDataSource {
public:
    struct SymSpec {
        std::string symbol;
        double start_mid;
        double spread;      // absolute top-of-book spread
        double vol;         // per-tick lognormal vol of the mid
    };

    MockMarketData(std::vector<SymSpec> specs, int max_ticks, unsigned seed = 42);

    bool next(Quote& out) override;
    std::string describe() const override;

private:
    std::vector<SymSpec> specs_;
    std::vector<double> mids_;
    int max_ticks_;
    int emitted_ = 0;
    std::mt19937 rng_;
    std::normal_distribution<double> norm_{0.0, 1.0};
};

}  // namespace el
