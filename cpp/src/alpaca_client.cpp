#include "execution/alpaca_client.hpp"

#include <chrono>
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
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

AlpacaClient::AlpacaClient()
    : key_(getenv_or("ALPACA_API_KEY_ID")), secret_(getenv_or("ALPACA_API_SECRET_KEY")) {}

bool AlpacaClient::get(const std::string& path, std::string& body) {
    if (!configured()) { err_ = "ALPACA_API_KEY_ID/ALPACA_API_SECRET_KEY not set"; return false; }

    CURL* curl = curl_easy_init();
    if (!curl) { err_ = "curl_easy_init failed"; return false; }

    const std::string url = "https://data.alpaca.markets" + path;
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

std::optional<Quote> AlpacaClient::latest_quote(const std::string& symbol) {
    std::string body;
    // feed=iex: free/Basic accounts only have IEX access. Omitting `feed`
    // defaults to the consolidated SIP feed, which requires a paid
    // subscription and returns 403 for Basic accounts.
    if (!get("/v2/stocks/" + symbol + "/quotes/latest?feed=iex", body)) return std::nullopt;

    try {
        json j = json::parse(body);
        const json& q = j.at("quote");
        Quote out;
        out.symbol = symbol;
        out.bid = q.value("bp", 0.0);
        out.ask = q.value("ap", 0.0);
        out.bsize = q.value("bs", 0.0);
        out.asize = q.value("as", 0.0);
        out.ts_ns = now_ns();  // receipt time; exact exchange ts not needed for display
        out.asset_class = AssetClass::Stock;
        return out;
    } catch (const std::exception& e) {
        err_ = std::string("alpaca parse error: ") + e.what();
        return std::nullopt;
    }
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

}  // namespace el
