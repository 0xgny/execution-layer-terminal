// ============================================================================
// kdb_client.hpp -- Minimal native KDB+ IPC client (arm64 macOS friendly).
//
// KX's prebuilt client object `c.o` isn't published for arm64 macOS, so we
// implement the tiny q login handshake ourselves and reuse the data-plane
// functions `k` / `r0` that ARE exported by the bundled libq.dylib (m64arm).
// This makes the C++ terminal a first-class KDB+ client with no external SDK.
//
// Threading: NOT thread-safe. All calls must come from a single owner thread
// (the engine/market-data thread). The GUI never calls this directly.
//
// Runtime: because we link libq, the process must be started with QHOME/QLIC set
// (same as running `q`). libq prints a one-time banner on load.
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "execution/types.hpp"

namespace el {

class KdbClient {
public:
    KdbClient() = default;
    ~KdbClient();
    KdbClient(const KdbClient&) = delete;
    KdbClient& operator=(const KdbClient&) = delete;

    // Open a TCP connection and perform the q IPC login handshake.
    bool connect(const std::string& host, int port, const std::string& creds = "el");
    bool connected() const { return fd_ >= 0; }
    void disconnect();

    // Poll the RDB's snap[syms] and parse into quotes (one per symbol with data).
    std::vector<Quote> snapshot(const std::vector<std::string>& symbols);

    // Recent last-trade prices for a symbol (RDB hist[sym;n]) for charting.
    std::vector<double> history(const std::string& symbol, int n);

    // Catalog of tradable products (tickerplant `products`).
    std::vector<std::string> products();

    // Control plane: ask the feed to start streaming a symbol (tickerplant).
    bool add_symbol(const std::string& symbol);

    // Fire-and-forget async statement.
    bool send_async(const std::string& expr);

    const std::string& last_error() const { return err_; }

private:
    // Send a sync query and read the raw response payload. false on I/O error.
    bool exec_sync(const std::string& expr, std::vector<unsigned char>& payload);
    std::vector<std::string> query_symbols(const std::string& expr);
    std::vector<double> query_floats(const std::string& expr);

    int fd_ = -1;
    std::string err_;
};

}  // namespace el
