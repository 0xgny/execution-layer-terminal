// ============================================================================
// selftest.cpp -- Offline end-to-end check of the execution pipeline.
//
//   Quote --> Portfolio + OMS + PaperMatchingEngine --> PnL
//
// Funds a $10k account, buys ~$3000 of BTC-USD at a synthetic price, marks the
// position as the market moves, then flattens and checks the realized PnL is
// what the arithmetic says it should be.
//
// This used to connect to a live KDB+ RDB, which meant it could only run with
// the whole stack up and never actually asserted anything. It is now
// deterministic, needs no network, and fails loudly.
//
//   ./build/selftest
// ============================================================================
#include <cmath>
#include <cstdio>
#include <map>
#include <string>

#include "execution/matching.hpp"
#include "execution/oms.hpp"
#include "execution/portfolio.hpp"
#include "execution/risk.hpp"
#include "execution/types.hpp"

using namespace el;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    std::printf("  [%s] %s\n", cond ? " ok " : "FAIL", what.c_str());
    if (!cond) ++failures;
}

void check_near(double got, double want, double tol, const std::string& what) {
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("  [%s] %s (got %.6f, want %.6f)\n", ok ? " ok " : "FAIL",
                what.c_str(), got, want);
    if (!ok) ++failures;
}

Quote quote_at(const std::string& sym, double bid, double ask) {
    Quote q;
    q.symbol = sym;
    q.bid = bid;
    q.ask = ask;
    q.bsize = 10.0;
    q.asize = 10.0;
    q.ts_ns = 1;
    return q;
}

}  // namespace

int main() {
    std::printf("[selftest] offline execution pipeline\n");

    Portfolio pf;
    pf.fund(10'000.0);
    RiskLimits lim;
    lim.max_order_qty = 1.0;
    lim.max_position_qty = 5.0;
    lim.max_order_notional = 1e6;
    RiskManager risk(lim);
    PaperMatchingEngine matcher;
    OrderManager oms(risk, matcher, pf);

    // --- buy ~$3000 at an ask of 60,000 -------------------------------------
    const Quote entry = quote_at("BTC-USD", 59'990.0, 60'000.0);
    const double qty = 3000.0 / entry.ask;  // 0.05 BTC

    auto buy = oms.submit(
        Signal{"BTC-USD", Side::Buy, qty, OrderType::Market, 0.0, "selftest", 0}, entry);
    check(buy.has_value(), "buy accepted");
    check(oms.orders().at(*buy).status == OrderStatus::Filled, "buy filled");
    check_near(pf.net_qty("BTC-USD"), qty, 1e-12, "position is 0.05 BTC");
    check_near(pf.cash(), 10'000.0 - 3000.0, 1e-9, "cash reduced by notional");

    // --- market moves up 10%: unrealized should be +$300 --------------------
    MarkMap marks{{"BTC-USD", 66'000.0}};
    check_near(pf.unrealized(marks), qty * (66'000.0 - 60'000.0), 1e-9, "unrealized +$300");
    check_near(pf.total_pnl(marks), 300.0, 1e-9, "total PnL +$300");

    // --- flatten into a bid of 66,000 ---------------------------------------
    const Quote exit = quote_at("BTC-USD", 66'000.0, 66'010.0);
    auto flat = oms.flatten("BTC-USD", exit);
    check(flat.has_value(), "flatten accepted");
    check_near(pf.net_qty("BTC-USD"), 0.0, 1e-12, "position closed");
    check_near(pf.realized(), 300.0, 1e-9, "realized +$300");
    check_near(pf.cash(), 10'300.0, 1e-9, "cash back to 10,300");
    check(pf.closed_positions().size() == 1, "one round trip recorded");

    // --- gates --------------------------------------------------------------
    auto too_big = oms.submit(
        Signal{"BTC-USD", Side::Buy, 2.0, OrderType::Market, 0.0, "selftest", 0}, entry);
    check(too_big && oms.orders().at(*too_big).status == OrderStatus::Rejected,
          "order above max_order_qty rejected");

    auto no_shorting = oms.submit(
        Signal{"BTC-USD", Side::Sell, 0.5, OrderType::Market, 0.0, "selftest", 0}, entry);
    check(no_shorting && oms.orders().at(*no_shorting).status == OrderStatus::Rejected,
          "sell with no position rejected");

    risk.kill("selftest");
    auto after_kill = oms.submit(
        Signal{"BTC-USD", Side::Buy, 0.01, OrderType::Market, 0.0, "selftest", 0}, entry);
    check(after_kill && oms.orders().at(*after_kill).status == OrderStatus::Rejected,
          "kill switch denies subsequent orders");

    std::printf("[selftest] %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
