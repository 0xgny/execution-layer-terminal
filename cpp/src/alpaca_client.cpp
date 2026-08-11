#include "execution/alpaca_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <curl/curl.h>

#include "json.hpp"

namespace el {

namespace {

using json = nlohmann::json;

std::string getenv_or(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

TimestampNs now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// "YYYY-MM-DD" for `n_days_back` calendar days before today (UTC), for the
// bars endpoint's `start` param. Alpaca defaults `start` to *today* when it's
// omitted, which silently returns only today's (possibly incomplete) bar
// instead of a history window -- always pass this explicitly.
std::string start_date_iso(int n_days_back) {
    std::time_t t = std::time(nullptr) - static_cast<std::time_t>(n_days_back) * 86400;
    std::tm tm{};
    gmtime_r(&t, &tm);
    // GCC's -Wformat-truncation can't prove tm_year won't be some huge value
    // (it theoretically can, per struct tm's contract), so size generously
    // rather than exactly -- this is always a real calendar date in practice.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

// Parse an Alpaca RFC-3339 UTC timestamp ("2026-08-10T17:16:00Z", sometimes
// with fractional seconds) to unix nanoseconds. 0 if it doesn't parse -- the
// caller only uses these to space points on a time axis, so a bad timestamp
// should drop that point's position, never fail the whole fetch.
TimestampNs iso_to_ns(const std::string& s) {
    std::tm tm{};
    int year, mon, day, hour, min, sec;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6)
        return 0;
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    const std::time_t t = timegm(&tm);  // UTC, unlike mktime
    if (t == static_cast<std::time_t>(-1)) return 0;
    return static_cast<TimestampNs>(t) * 1'000'000'000LL;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

AlpacaClient::AlpacaClient()
    : key_(getenv_or("ALPACA_API_KEY_ID")), secret_(getenv_or("ALPACA_API_SECRET_KEY")) {}

bool AlpacaClient::get(const std::string& path, std::string& body, const char* host) {
    if (!configured()) { err_ = "ALPACA_API_KEY_ID/ALPACA_API_SECRET_KEY not set"; return false; }

    CURL* curl = curl_easy_init();
    if (!curl) { err_ = "curl_easy_init failed"; return false; }

    const std::string url = std::string(host) + path;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("APCA-API-KEY-ID: " + key_).c_str());
    headers = curl_slist_append(headers, ("APCA-API-SECRET-KEY: " + secret_).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { err_ = std::string("curl error: ") + curl_easy_strerror(res); return false; }
    if (status < 200 || status >= 300) {
        err_ = "alpaca HTTP " + std::to_string(status) + ": " + body;
        return false;
    }
    return true;
}

std::map<std::string, Quote> AlpacaClient::latest_quotes(const std::vector<std::string>& symbols) {
    if (symbols.empty()) return {};

    std::string list;
    for (const std::string& s : symbols) {
        if (!list.empty()) list += ",";
        list += s;
    }

    std::string body;
    // feed=iex: free/Basic accounts only have IEX access. Omitting `feed`
    // defaults to the consolidated SIP feed, which requires a paid
    // subscription and returns 403 for Basic accounts.
    if (!get("/v2/stocks/quotes/latest?feed=iex&symbols=" + list, body)) return {};

    try {
        json j = json::parse(body);
        const auto quotes = j.find("quotes");
        if (quotes == j.end() || !quotes->is_object()) return {};

        const TimestampNs recv = now_ns();  // receipt time; exact exchange ts not needed for display
        std::map<std::string, Quote> out;
        for (auto it = quotes->begin(); it != quotes->end(); ++it) {
            if (!it.value().is_object()) continue;
            Quote q;
            q.symbol = it.key();
            q.bid = it.value().value("bp", 0.0);
            q.ask = it.value().value("ap", 0.0);
            q.bsize = it.value().value("bs", 0.0);
            q.asize = it.value().value("as", 0.0);
            q.ts_ns = recv;
            out[q.symbol] = q;
        }
        return out;
    } catch (const std::exception& e) {
        err_ = std::string("alpaca parse error: ") + e.what();
        return {};
    }
}

std::optional<Quote> AlpacaClient::latest_quote(const std::string& symbol) {
    auto quotes = latest_quotes({symbol});
    auto it = quotes.find(symbol);
    if (it == quotes.end()) return std::nullopt;
    return it->second;
}

std::vector<double> AlpacaClient::daily_bars(const std::string& symbol, int n) {
    std::string body;
    // Look back generously past `n` trading days (weekends/holidays eat ~2/7
    // of calendar days) so `limit` has enough of a window to actually fill.
    const std::string start = start_date_iso(n * 2 + 30);
    const std::string path = "/v2/stocks/" + symbol + "/bars?timeframe=1Day&limit=" +
                              std::to_string(n) + "&adjustment=raw&feed=iex&start=" + start;
    if (!get(path, body)) return {};

    try {
        json j = json::parse(body);
        std::vector<double> closes;
        for (const auto& bar : j.at("bars")) closes.push_back(bar.value("c", 0.0));
        return closes;  // Alpaca returns bars oldest-first for a plain limit query
    } catch (const std::exception& e) {
        err_ = std::string("alpaca parse error: ") + e.what();
        return {};
    }
}

std::vector<PricePoint> AlpacaClient::recent_minute_bars(const std::string& symbol, int n) {
    if (n <= 0) return {};

    std::string body;
    // sort=desc gives the *latest* n bars in one request. Ascending (the
    // default) would return the oldest n from whatever `start` we guessed, so
    // getting recent data that way means either paging or over-fetching a whole
    // session and throwing most of it away.
    const std::string path = "/v2/stocks/" + symbol + "/bars?timeframe=1Min&limit=" +
                             std::to_string(n) + "&adjustment=raw&feed=iex&sort=desc";
    if (!get(path, body)) return {};

    try {
        json j = json::parse(body);
        const auto bars = j.find("bars");
        if (bars == j.end() || !bars->is_array()) return {};

        std::vector<PricePoint> out;
        out.reserve(bars->size());
        for (const auto& bar : *bars) {
            const double close = bar.value("c", 0.0);
            if (!(close > 0.0)) continue;
            out.push_back({iso_to_ns(bar.value("t", std::string{})), close});
        }
        // Undo sort=desc: everything downstream wants oldest-first.
        std::reverse(out.begin(), out.end());
        return out;
    } catch (const std::exception& e) {
        err_ = std::string("alpaca parse error: ") + e.what();
        return {};
    }
}

std::string AlpacaClient::asset_name(const std::string& symbol) {
    std::string body;
    if (!get("/v2/assets/" + symbol, body, kTradingHost)) return {};
    auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return {};
    auto it = j.find("name");
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

}  // namespace el
