#include "execution/matching.hpp"

namespace el {

std::vector<Fill> PaperMatchingEngine::fill(const Order& order, const Quote& market) {
    std::vector<Fill> fills;
    const double qty = order.leaves();
    if (!(qty > 0.0)) return fills;

    // Price we would trade at if crossing the spread now. Falls back to the
    // other side's real quote (see Quote::touch) rather than blocking the
    // fill outright when a single-venue feed shows a momentarily one-sided book.
    const double touch = market.touch(order.side);
    if (!(touch > 0.0)) return fills;  // truly no quote at all -> no fill

    if (order.type == OrderType::Limit) {
        // Only fill if the limit is marketable against the touch.
        const bool marketable = order.side == Side::Buy ? order.limit_price >= market.ask
                                                        : order.limit_price <= market.bid;
        if (!marketable) return fills;  // rest (unmodeled) -> no fill this tick
    }

    fills.push_back(Fill{
        /*order_id*/ order.id,
        /*symbol  */ order.symbol,
        /*side    */ order.side,
        /*qty     */ qty,
        /*price   */ touch,
        /*ts_ns   */ market.ts_ns + (++seq_),
    });
    return fills;
}

}  // namespace el
