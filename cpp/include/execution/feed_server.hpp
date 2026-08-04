// ============================================================================
// feed_server.hpp -- Localhost socket the Python feedhandler publishes into.
//
// Replaces the tickerplant. The feedhandler connects here and writes
// newline-delimited JSON batches; we decode them straight into a MarketStore.
// One publisher at a time, which is all there has ever been.
//
// Wire format (one JSON object per line, UTF-8):
//
//   feed -> app
//     {"m":"quotes","rows":[[sym,bid,ask,bsize,asize,ts_ns], ...]}
//     {"m":"trades","rows":[[sym,price,ts_ns], ...]}
//     {"m":"products","syms":[...]}      catalog for Ticker Search
//     {"m":"universe","syms":[...]}      what the feed is actually streaming
//     {"m":"poll"}                       "any symbols requested?"
//   app -> feed
//     {"m":"requested","syms":[...]}     reply to poll (may be empty)
//
// JSON rather than a binary frame is deliberate: at ~100 symbols on a 250 ms
// flush this is nowhere near a bottleneck, and the feed can be inspected with
// `nc`. Swap the codec if profiling ever says otherwise -- nothing above this
// file knows the format.
// ============================================================================
#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace el {

class MarketStore;

class FeedServer {
public:
    FeedServer(MarketStore& store, int port) : store_(store), port_(port) {}
    ~FeedServer();
    FeedServer(const FeedServer&) = delete;
    FeedServer& operator=(const FeedServer&) = delete;

    // Bind, listen, and spawn the accept thread. False if the port is taken.
    bool start();
    void stop();

    const std::string& last_error() const { return err_; }
    int port() const { return port_; }

private:
    void run();                                  // accept loop
    void serve(int client_fd);                   // read loop for one publisher
    void handle_line(const std::string& line, int client_fd);

    MarketStore& store_;
    int port_;
    int listen_fd_ = -1;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::string err_;
};

}  // namespace el
