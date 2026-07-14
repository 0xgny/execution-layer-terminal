// ============================================================================
// main.cpp -- Execution Layer demo harness.
//
// Wires the full paper pipeline end to end with no external dependencies:
//
//   MockMarketData -> MovingAverageCross -> OrderManager
//                                             |-> RiskManager (pre-trade gate)
//                                             `-> PaperMatchingEngine (fills)
//                                                   -> Positions / PnL
//
// and renders a Bloomberg-terminal-style text blotter. This is the seed the
// Dear ImGui + ImPlot GUI will grow from; the same OrderManager state feeds it.
//
// Later, MockMarketData is replaced by a KDB+ source (subscribing to the RDB's
// quote table) and signals arrive from the KDB+ `signal` table -- the C++ core
// here is unchanged.
// ============================================================================
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "execution/market_data.hpp"
#include "execution/oms.hpp"
#include "execution/risk.hpp"
#include "execution/strategy.hpp"
#include "execution/types.hpp"

namespace {

// --- Bloomberg-ish terminal palette (ANSI) ---------------------------------
namespace ansi {
constexpr const char* reset = "\033[0m";
constexpr const char* amber = "\033[38;5;214m";
constexpr const char* dim = "\033[38;5;244m";
constexpr const char* cyan = "\033[38;5;44m";
constexpr const char* green = "\033[38;5;40m";
constexpr const char* red = "\033[38;5;196m";
constexpr const char* white = "\033[38;5;231m";
constexpr const char* bold = "\033[1m";
}  // namespace ansi

std::string money(double v, bool color = false) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << (v >= 0 ? " " : "") << v;
    if (!color) return os.str();
    return std::string(v >= 0 ? ansi::green : ansi::red) + os.str() + ansi::reset;
}

void banner(const std::string& title) {
    std::cout << ansi::amber << ansi::bold
              << "\n╔══════════════════════════════════════════════════════════════════╗\n"
              << "║  " << std::left << std::setw(64) << title << "║\n"
              << "╚══════════════════════════════════════════════════════════════════╝"
              << ansi::reset << "\n";
}

void print_blotter(el::OrderManager& oms, const std::map<std::string, double>& marks) {
    banner("EXECUTION LAYER  //  PAPER BLOTTER");

    // Positions + PnL
    std::cout << ansi::cyan << ansi::bold
              << std::left << std::setw(10) << "SYMBOL" << std::right
              << std::setw(12) << "NET" << std::setw(14) << "AVG PX"
              << std::setw(14) << "MARK" << std::setw(14) << "REAL PnL"
              << std::setw(14) << "UNREAL" << std::setw(14) << "TOTAL"
              << ansi::reset << "\n";

    double tot_real = 0.0, tot_unreal = 0.0;
    for (const auto& [sym, pos] : oms.portfolio().positions()) {
        const double mark = marks.count(sym) ? marks.at(sym) : pos.avg_price;
        const double un = pos.unrealized(mark);
        tot_real += pos.realized_pnl;
        tot_unreal += un;
        std::cout << ansi::white << std::left << std::setw(10) << sym << std::right
                  << std::setw(12) << std::fixed << std::setprecision(4) << pos.net_qty
                  << std::setw(14) << std::setprecision(2) << pos.avg_price
                  << std::setw(14) << mark
                  << std::setw(14) << money(pos.realized_pnl, false)
                  << std::setw(14) << money(un, false)
                  << std::setw(14) << money(pos.realized_pnl + un, false)
                  << ansi::reset << "\n";
    }
    std::cout << ansi::dim
              << "──────────────────────────────────────────────────────────────────\n"
              << ansi::reset << ansi::bold << "TOTAL   "
              << "realized " << money(tot_real, true)
              << ansi::bold << "   unrealized " << money(tot_unreal, true)
              << ansi::bold << "   equity " << money(tot_real + tot_unreal, true)
              << ansi::reset << "\n";

    // Order status tally
    std::map<std::string, int> tally;
    for (const auto& [id, o] : oms.orders()) tally[el::to_string(o.status)]++;
    std::cout << ansi::dim << "\norders: ";
    for (const auto& [st, n] : tally) std::cout << st << "=" << n << "  ";
    std::cout << "| fills: " << oms.fills().size() << ansi::reset << "\n";
}

}  // namespace

int main() {
    using namespace el;

    banner("EXECUTION LAYER  //  boot");
    std::cout << ansi::dim << "paper trading; no real orders are routed.\n" << ansi::reset;

    // --- wire the components -------------------------------------------------
    RiskLimits limits;
    limits.max_order_qty = 0.50;
    limits.max_position_qty = 1.00;
    limits.max_order_notional = 60'000.0;
    RiskManager risk(limits);

    PaperMatchingEngine matcher;
    Portfolio portfolio;
    portfolio.fund(100'000.0);
    OrderManager oms(risk, matcher, portfolio);

    MockMarketData md({{"BTC-USD", 62'000.0, 2.0, 0.0010},
                       {"ETH-USD", 1'770.0, 0.20, 0.0012}},
                      /*max_ticks*/ 400);
    MovingAverageCross strat(/*fast*/ 5, /*slow*/ 20, /*order_qty*/ 0.10);

    std::cout << ansi::dim << "source: " << md.describe()
              << " | strategy: " << strat.name()
              << " | limits: maxOrd=" << limits.max_order_qty
              << " maxPos=" << limits.max_position_qty << ansi::reset << "\n";

    // --- run the sim ---------------------------------------------------------
    std::map<std::string, double> marks;
    Quote q;
    while (md.next(q)) {
        marks[q.symbol] = q.mid();
        if (auto sig = strat.on_quote(q)) {
            auto id = oms.submit(*sig, q);
            if (id) {
                const Order& o = oms.orders().at(*id);
                const char* col = o.side == Side::Buy ? ansi::green : ansi::red;
                std::cout << col << "  signal " << to_string(o.side) << " "
                          << std::fixed << std::setprecision(3) << o.qty << " "
                          << o.symbol << " @mid " << std::setprecision(2) << q.mid()
                          << "  -> order#" << o.id << " " << to_string(o.status);
                if (o.status == OrderStatus::Filled)
                    std::cout << " @" << o.avg_fill_price;
                if (o.status == OrderStatus::Rejected)
                    std::cout << " (" << o.reject_reason << ")";
                std::cout << ansi::reset << "\n";
            }
        }
    }

    print_blotter(oms, marks);

    // --- risk demo: show the gate + kill switch actually blocking ------------
    banner("RISK CONTROLS  //  demonstration");
    Quote btc{"BTC-USD", 61'999.0, 62'001.0, 1, 1, q.ts_ns};

    Signal oversize{"BTC-USD", Side::Buy, 5.0, OrderType::Market, 0, "manual", q.ts_ns};
    if (auto id = oms.submit(oversize, btc)) {
        const Order& o = oms.orders().at(*id);
        std::cout << ansi::red << "  oversized order#" << o.id << " -> "
                  << to_string(o.status) << " (" << o.reject_reason << ")"
                  << ansi::reset << "\n";
    }

    std::cout << ansi::amber << "  tripping kill switch...\n" << ansi::reset;
    risk.kill("manual halt (demo)");
    Signal normal{"BTC-USD", Side::Buy, 0.10, OrderType::Market, 0, "manual", q.ts_ns};
    if (auto id = oms.submit(normal, btc)) {
        const Order& o = oms.orders().at(*id);
        std::cout << ansi::red << "  normal order#" << o.id << " -> "
                  << to_string(o.status) << " (" << o.reject_reason << ")"
                  << ansi::reset << "\n";
    }

    std::cout << ansi::dim << "\ndone.\n" << ansi::reset;
    return 0;
}
