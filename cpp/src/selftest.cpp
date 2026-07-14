// ============================================================================
// selftest.cpp -- Headless end-to-end check of the live trading pipeline.
//
//   KdbClient (RDB) --> live quotes --> Portfolio + OMS + PaperMatchingEngine
//
// Connects to the RDB, funds a $10k account, buys ~$3000 of BTC-USD at the live
// price, prints real-time PnL for a few seconds as the market moves, then
// flattens. This verifies the whole M1/M2 wiring without needing a GUI/display.
//
//   ./build/selftest [host] [port]        (defaults 127.0.0.1 5011)
// ============================================================================
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

#include "execution/kdb_client.hpp"
#include "execution/matching.hpp"
#include "execution/oms.hpp"
#include "execution/portfolio.hpp"
#include "execution/risk.hpp"
#include "execution/types.hpp"

using namespace el;

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? std::atoi(argv[2]) : 5011;
    const std::vector<std::string> watch = {"BTC-USD", "ETH-USD"};

    KdbClient kdb;
    if (!kdb.connect(host, port)) {
        printf("[selftest] connect failed: %s\n", kdb.last_error().c_str());
        return 1;
    }
    printf("[selftest] connected to RDB %s:%d\n", host, port);

    Portfolio pf;
    pf.fund(10'000.0);
    RiskLimits lim;
    lim.max_order_qty = 1.0;
    lim.max_position_qty = 5.0;
    lim.max_order_notional = 1e6;
    RiskManager risk(lim);
    PaperMatchingEngine matcher;
    OrderManager oms(risk, matcher, pf);

    printf("[selftest] funded $%.2f; buying ~$3000 of BTC-USD live, then flattening.\n",
           pf.initial_capital());

    MarkMap marks;
    std::map<std::string, Quote> q;
    bool bought = false;

    for (int i = 0; i < 16; ++i) {
        auto qs = kdb.snapshot(watch);
        if (qs.empty()) {
            printf("  (no data: %s)\n", kdb.last_error().c_str());
            usleep(500'000);
            continue;
        }
        for (auto& x : qs) { q[x.symbol] = x; marks[x.symbol] = x.mid(); }

        if (i == 2 && q.count("BTC-USD")) {
            const double ask = q["BTC-USD"].ask;
            const double qty = 3000.0 / ask;
            Signal s{"BTC-USD", Side::Buy, qty, OrderType::Market, 0.0, "selftest", 0};
            if (auto id = oms.submit(s, q["BTC-USD"])) {
                const Order& o = oms.orders().at(*id);
                printf("  >>> BUY %.6f BTC @ %.2f -> %s", qty, ask, to_string(o.status));
                if (o.status == OrderStatus::Rejected) printf(" (%s)", o.reject_reason.c_str());
                printf("\n");
                bought = (o.status == OrderStatus::Filled);
            }
        }
        if (i == 12 && bought) {
            if (auto id = oms.flatten("BTC-USD", q["BTC-USD"])) {
                const Order& o = oms.orders().at(*id);
                printf("  <<< FLATTEN BTC-USD -> %s @ %.2f\n", to_string(o.status), o.avg_fill_price);
            }
        }

        const double mid = marks.count("BTC-USD") ? marks["BTC-USD"] : 0.0;
        printf("  t=%2d  BTC mid=%.2f  pos=%.6f  cash=%.2f  equity=%.2f  PnL=%+.2f\n",
               i, mid, pf.net_qty("BTC-USD"), pf.cash(), pf.equity(marks), pf.total_pnl(marks));
        usleep(600'000);
    }

    printf("[selftest] done. realized=%.2f  final equity=%.2f  total PnL=%+.2f\n",
           pf.realized(), pf.equity(marks), pf.total_pnl(marks));
    return 0;
}
