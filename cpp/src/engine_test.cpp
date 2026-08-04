// Headless test of the threaded TradingEngine against a live feedhandler: fund,
// buy, watch PnL via the published view, flatten. Verifies the engine thread +
// the command/view handoff the GUI relies on.
//
// Run `python -m feedhandler --venue mock` alongside this.
//   ./build/engine_test [feed_port]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "execution/engine.hpp"

using namespace el;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char** argv) {
    const int feed_port = argc > 1 ? std::atoi(argv[1]) : 5020;

    // Sized for the mock venue, which quotes BTC-USD around $200 -- a $2500
    // order is ~13 units there, so the old max_order_qty of 1.0 meant this test
    // silently rejected its own buy and never exercised a fill.
    RiskLimits lim;
    lim.max_order_qty = 100.0;
    lim.max_position_qty = 500.0;
    lim.max_order_notional = 1e6;

    TradingEngine eng(feed_port, {"BTC-USD", "ETH-USD"}, 10'000.0, lim);
    eng.start();
    sleep_ms(1500);

    printf("[engine_test] BUY $2500 BTC-USD\n");
    eng.post({Command::Buy, "BTC-USD", 2500.0, true});
    sleep_ms(1500);

    // Control plane: ask the feed to start streaming a symbol we didn't boot
    // with. The feedhandler polls for this and live-subscribes on the open
    // WebSocket, so a quote for it should appear without restarting anything.
    printf("[engine_test] ADD SOL-USD (control plane)\n");
    eng.post({Command::AddSymbol, "SOL-USD", 0, false, AssetClass::Crypto});

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
    for (auto& s : v.symbols)
        if (s.symbol == "SOL-USD")
            printf("[engine_test] control plane: SOL-USD %s\n",
                   s.has_quote ? "streaming (quote received)" : "requested, no quote yet");
    if (!v.products.empty())
        printf("[engine_test] sample products: %s %s %s ...\n",
               v.products[0].c_str(), v.products.size() > 1 ? v.products[1].c_str() : "",
               v.products.size() > 2 ? v.products[2].c_str() : "");
    printf("[engine_test] event log:\n");
    for (auto& l : v.log) printf("   %s\n", l.c_str());

    eng.stop();
    return 0;
}
