// ============================================================================
// alpaca_test.cpp -- headless smoke test for AlpacaClient.
//
// Verifies the HTTP + auth + JSON-parsing path against the real Alpaca data
// API, with no GUI/display needed. Requires ALPACA_API_KEY_ID and
// ALPACA_API_SECRET_KEY in the environment; without them it reports that
// clearly and exits nonzero rather than silently passing.
// ============================================================================
#include <cstdio>
#include <cstdlib>

#include "execution/alpaca_client.hpp"

using namespace el;

int main(int argc, char** argv) {
    const std::string symbol = argc > 1 ? argv[1] : "AAPL";

    AlpacaClient client;
    if (!client.configured()) {
        std::fprintf(stderr,
            "ALPACA_API_KEY_ID / ALPACA_API_SECRET_KEY not set -- nothing to test.\n");
        return 1;
    }

    std::printf("fetching latest quote for %s...\n", symbol.c_str());
    auto q = client.latest_quote(symbol);
    if (!q) {
        std::fprintf(stderr, "latest_quote failed: %s\n", client.last_error().c_str());
        return 1;
    }
    std::printf("  bid=%.4f ask=%.4f bsize=%.2f asize=%.2f\n", q->bid, q->ask, q->bsize, q->asize);

    std::printf("fetching daily bars for %s...\n", symbol.c_str());
    auto closes = client.daily_bars(symbol, 10);
    if (closes.empty()) {
        std::fprintf(stderr, "daily_bars failed: %s\n", client.last_error().c_str());
        return 1;
    }
    std::printf("  %zu closes, most recent = %.4f\n", closes.size(), closes.back());

    std::printf("OK\n");
    return 0;
}
