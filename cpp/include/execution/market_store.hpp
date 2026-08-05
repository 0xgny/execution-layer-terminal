// ============================================================================
// market_store.hpp -- In-process market-data store.
//
// This replaces the KDB+ tickerplant + RDB pair. Those two processes existed to
// serve exactly two reads: a per-symbol last-value lookup (`snap`) and the last
// N trade prices for one symbol (`hist`). That is a hash map and a ring buffer,
// so this is a hash map and a ring buffer.
//
// Deliberately NOT stored: the full trade/quote tape. The RDB accumulated every
// tick forever with no end-of-day flush, and nothing ever read it -- the `quote`
// table was write-only and `trade` was read only for the last 300 prices of the
// charted symbol. Bounding history per symbol makes that leak structurally
// impossible.
//
// Threading: the FeedServer thread calls the writer half, the engine thread
// calls the reader half, and one mutex covers both. Held only for map lookups
// and small copies -- never across I/O.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "execution/types.hpp"

namespace el {

class MarketStore {
public:
    // Trade prices retained per symbol. The chart asks for 300 (engine.cpp);
    // the slack absorbs a burst without reallocating.
    static constexpr std::size_t kHistoryCap = 512;

    // --- writer half: called from the FeedServer thread ---------------------
    void on_quote(const Quote& q);
    void on_trade(const std::string& symbol, double price);
    void set_products(std::vector<std::string> products);
    void set_universe(std::vector<std::string> universe);
    void set_feed_attached(bool attached) { feed_attached_ = attached; }

    // Drain the symbols the terminal has asked to start streaming. Draining
    // (rather than accumulating, as the tickerplant's `requested` vector did)
    // means the feedhandler can't re-subscribe to the same symbol on every poll.
    std::vector<std::string> take_requested();

    // --- reader half: called from the engine thread -------------------------
    bool connected() const { return feed_attached_; }

    // Latest top-of-book for each requested symbol. Symbols that haven't ticked
    // yet are omitted rather than returned empty, so the caller can tell "no
    // data yet" from "quoted at zero" -- the GUI shows "..." for the former.
    std::vector<Quote> snapshot(const std::vector<std::string>& symbols) const;

    // Last `n` trade prices for `symbol`, oldest first.
    std::vector<double> history(const std::string& symbol, int n) const;

    std::vector<std::string> products() const;
    std::vector<std::string> universe() const;

    // Ask the feed to start streaming `symbol` (was `.u.addsym` on the tp).
    void add_symbol(const std::string& symbol);

private:
    mutable std::mutex mu_;
    std::map<std::string, Quote> last_quote_;
    std::map<std::string, std::deque<double>> history_;
    std::vector<std::string> products_;
    std::vector<std::string> universe_;
    std::vector<std::string> requested_;

    // Read by the engine every cycle without taking the mutex, so it's atomic.
    std::atomic<bool> feed_attached_{false};
};

}  // namespace el
