// Headless test of the threaded TradingEngine against a live RDB: fund, buy,
// watch PnL via the published view, flatten. Verifies the engine thread + the
// command/view handoff the GUI relies on.  ./build/engine_test [host] [port]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "execution/engine.hpp"

using namespace el;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "127.0.0.1";
    const int port = argc > 2 ? std::atoi(argv[2]) : 5011;
    const int tp_port = argc > 3 ? std::atoi(argv[3]) : 5010;

    RiskLimits lim;
    lim.max_order_qty = 1.0;
    lim.max_position_qty = 5.0;
    lim.max_order_notional = 1e6;

    TradingEngine eng(host, port, tp_port, {"BTC-USD", "ETH-USD"}, 10'000.0, lim);
    eng.start();
    sleep_ms(1500);

    printf("[engine_test] BUY $2500 BTC-USD\n");
    eng.post({Command::Buy, "BTC-USD", 2500.0, true});
    sleep_ms(1500);

    for (int i = 0; i < 5; ++i) {
        EngineView v = eng.view();
        double mid = 0, pos = 0, un = 0;
        for (auto& s : v.symbols)
            if (s.symbol == "BTC-USD") { mid = s.mid; pos = s.net_qty; un = s.unrealized; }
        printf("  equity=%.2f pnl=%+.2f cash=%.2f | BTC mid=%.2f pos=%.6f unreal=%+.2f\n",
               v.equity, v.total_pnl, v.cash, mid, pos, un);
        sleep_ms(700);
    }

    printf("[engine_test] FLATTEN BTC-USD\n");
    eng.post({Command::Flatten, "BTC-USD", 0, false});
    sleep_ms(1200);

    EngineView v = eng.view();
    printf("[engine_test] cash=%.2f equity=%.2f realized=%.2f\n", v.cash, v.equity, v.realized);
    printf("[engine_test] catalog=%zu products | focus=%s price_history=%zu pnl_points=%zu\n",
           v.products.size(), v.focus.c_str(), v.price_history.size(), v.pnl_history.size());
    if (!v.products.empty())
        printf("[engine_test] sample products: %s %s %s ...\n",
               v.products[0].c_str(), v.products.size() > 1 ? v.products[1].c_str() : "",
               v.products.size() > 2 ? v.products[2].c_str() : "");
    printf("[engine_test] event log:\n");
    for (auto& l : v.log) printf("   %s\n", l.c_str());

    eng.stop();
    return 0;
}
