// Pure-socket KDB+ IPC client. Implements just enough of the q IPC protocol to
// log in, send synchronous string queries, and deserialize the result shapes we
// use: the snap table, symbol vectors (catalog), and float vectors (history).
//
// Why not link KX's C client? The prebuilt `c.o` isn't published for arm64
// macOS, and linking the full libq.dylib drags in the whole q runtime (it takes
// over the process on load). A small protocol client is cleaner, dependency
// free, and portable. Wire framing was verified byte-for-byte against a live q.
#include "execution/kdb_client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <map>

namespace el {
namespace {

// q IPC type codes we handle.
constexpr unsigned char T_ERROR = 128;  // -128 as unsigned
constexpr unsigned char T_LIST = 0;     // general list
constexpr unsigned char T_FLOATV = 9;   // float vector
constexpr unsigned char T_SYMV = 11;    // symbol vector
constexpr unsigned char T_DICT = 99;
constexpr unsigned char T_TABLE = 98;

bool recv_all(int fd, unsigned char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

struct Cursor {
    const unsigned char* p;
    const unsigned char* end;
    bool ok = true;

    unsigned char u8() { if (p >= end) { ok = false; return 0; } return *p++; }
    int32_t i32() {
        if (p + 4 > end) { ok = false; return 0; }
        int32_t v; std::memcpy(&v, p, 4); p += 4; return v;
    }
    double f64() {
        if (p + 8 > end) { ok = false; return 0; }
        double v; std::memcpy(&v, p, 8); p += 8; return v;
    }
    std::string cstr() {
        std::string s;
        while (p < end && *p) s.push_back(static_cast<char>(*p++));
        if (p < end) ++p; else ok = false;
        return s;
    }
};

// Frame a query string as a KC char-vector payload and send it.
bool send_query(int fd, const std::string& q, bool sync) {
    const uint32_t qn = static_cast<uint32_t>(q.size());
    const uint32_t msglen = 8 + 6 + qn;
    std::string buf;
    buf.reserve(msglen);
    buf.push_back(1);                 // little-endian
    buf.push_back(sync ? 1 : 0);      // sync / async
    buf.push_back(0);
    buf.push_back(0);
    buf.append(reinterpret_cast<const char*>(&msglen), 4);
    buf.push_back(10);                // KC char vector
    buf.push_back(0);
    buf.append(reinterpret_cast<const char*>(&qn), 4);
    buf.append(q);
    return ::send(fd, buf.data(), buf.size(), 0) == static_cast<ssize_t>(buf.size());
}

}  // namespace

KdbClient::~KdbClient() { disconnect(); }

void KdbClient::disconnect() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
}

bool KdbClient::connect(const std::string& host, int port, const std::string& creds) {
    disconnect();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err_ = "socket() failed"; return false; }

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &a.sin_addr) <= 0) {
        err_ = "bad host address"; ::close(fd); return false;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        err_ = "connect() refused"; ::close(fd); return false;
    }

    std::string msg = creds;
    msg.push_back('\x03');
    msg.push_back('\0');
    if (::send(fd, msg.data(), msg.size(), 0) != static_cast<ssize_t>(msg.size())) {
        err_ = "handshake send failed"; ::close(fd); return false;
    }
    unsigned char reply = 0;
    if (::recv(fd, &reply, 1, 0) != 1) {
        err_ = "handshake rejected (auth?)"; ::close(fd); return false;
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fd_ = fd;
    err_.clear();
    return true;
}

bool KdbClient::send_async(const std::string& expr) {
    if (fd_ < 0) return false;
    return send_query(fd_, expr, /*sync=*/false);
}

bool KdbClient::add_symbol(const std::string& symbol) {
    return send_async(".u.addsym[`$\"" + symbol + "\"]");
}

bool KdbClient::exec_sync(const std::string& expr, std::vector<unsigned char>& payload) {
    if (fd_ < 0) return false;
    if (!send_query(fd_, expr, /*sync=*/true)) { err_ = "send failed"; fd_ = -1; return false; }

    unsigned char hdr[8];
    if (!recv_all(fd_, hdr, 8)) { err_ = "no response (connection lost)"; fd_ = -1; return false; }
    uint32_t rlen;
    std::memcpy(&rlen, hdr + 4, 4);
    if (hdr[2] != 0) { err_ = "compressed response unsupported"; fd_ = -1; return false; }
    if (rlen < 8) { err_ = "short response"; return false; }
    payload.resize(rlen - 8);
    if (!recv_all(fd_, payload.data(), payload.size())) {
        err_ = "truncated response"; fd_ = -1; return false;
    }
    return true;
}

std::vector<Quote> KdbClient::snapshot(const std::vector<std::string>& symbols) {
    std::vector<Quote> out;
    if (fd_ < 0 || symbols.empty()) return out;

    // snap[...] with a string->symbol cast so dashed tickers (BTC-USD) work.
    std::string qy;
    if (symbols.size() == 1) {
        qy = "snap[`$\"" + symbols[0] + "\"]";
    } else {
        qy = "snap[`$(";
        for (std::size_t i = 0; i < symbols.size(); ++i) {
            qy += "\"" + symbols[i] + "\"";
            if (i + 1 < symbols.size()) qy += ";";
        }
        qy += ")]";
    }

    std::vector<unsigned char> payload;
    if (!exec_sync(qy, payload)) return out;

    Cursor c{payload.data(), payload.data() + payload.size()};
    unsigned char t = c.u8();
    if (t == T_ERROR) { err_ = "q error: " + c.cstr(); return out; }
    if (t != T_TABLE) { err_ = "snap: expected table"; return out; }
    c.u8();
    if (c.u8() != T_DICT) { err_ = "snap: expected dict"; return out; }

    if (c.u8() != T_SYMV) { err_ = "snap: bad colnames"; return out; }
    c.u8();
    int32_t ncols = c.i32();
    std::vector<std::string> names;
    for (int32_t i = 0; i < ncols && c.ok; ++i) names.push_back(c.cstr());

    if (c.u8() != T_LIST) { err_ = "snap: bad columns"; return out; }
    c.u8();
    int32_t ncols2 = c.i32();

    std::map<std::string, std::vector<std::string>> symcols;
    std::map<std::string, std::vector<double>> fltcols;
    for (int32_t col = 0; col < ncols2 && c.ok; ++col) {
        unsigned char ct = c.u8();
        c.u8();
        int32_t n = c.i32();
        const std::string& nm = (col < static_cast<int32_t>(names.size())) ? names[col] : "";
        if (ct == T_SYMV) {
            auto& v = symcols[nm];
            for (int32_t i = 0; i < n && c.ok; ++i) v.push_back(c.cstr());
        } else if (ct == T_FLOATV) {
            auto& v = fltcols[nm];
            for (int32_t i = 0; i < n && c.ok; ++i) v.push_back(c.f64());
        } else {
            err_ = "snap: unexpected column type"; return out;
        }
    }
    if (!c.ok) { err_ = "snap: parse overran buffer"; return out; }

    auto sym = symcols.find("sym");
    if (sym == symcols.end()) { err_ = "snap: no sym column"; return out; }
    const auto& syms = sym->second;
    auto col = [&](const char* nm) -> const std::vector<double>* {
        auto it = fltcols.find(nm);
        return it == fltcols.end() ? nullptr : &it->second;
    };
    const auto* bid = col("bid");
    const auto* ask = col("ask");
    const auto* bsz = col("bsize");
    const auto* asz = col("asize");

    out.reserve(syms.size());
    for (std::size_t i = 0; i < syms.size(); ++i) {
        Quote q;
        q.symbol = syms[i];
        q.bid = bid && i < bid->size() ? (*bid)[i] : 0.0;
        q.ask = ask && i < ask->size() ? (*ask)[i] : 0.0;
        q.bsize = bsz && i < bsz->size() ? (*bsz)[i] : 0.0;
        q.asize = asz && i < asz->size() ? (*asz)[i] : 0.0;
        out.push_back(std::move(q));
    }
    err_.clear();
    return out;
}

std::vector<std::string> KdbClient::query_symbols(const std::string& expr) {
    std::vector<std::string> out;
    std::vector<unsigned char> payload;
    if (!exec_sync(expr, payload)) return out;
    Cursor c{payload.data(), payload.data() + payload.size()};
    unsigned char t = c.u8();
    if (t == T_ERROR) { err_ = "q error: " + c.cstr(); return out; }
    if (t != T_SYMV) { err_ = "expected symbol vector"; return out; }
    c.u8();
    int32_t n = c.i32();
    for (int32_t i = 0; i < n && c.ok; ++i) out.push_back(c.cstr());
    return out;
}

std::vector<double> KdbClient::query_floats(const std::string& expr) {
    std::vector<double> out;
    std::vector<unsigned char> payload;
    if (!exec_sync(expr, payload)) return out;
    Cursor c{payload.data(), payload.data() + payload.size()};
    unsigned char t = c.u8();
    if (t == T_ERROR) { err_ = "q error: " + c.cstr(); return out; }
    if (t != T_FLOATV) { err_ = "expected float vector"; return out; }
    c.u8();
    int32_t n = c.i32();
    out.reserve(n);
    for (int32_t i = 0; i < n && c.ok; ++i) out.push_back(c.f64());
    return out;
}

std::vector<std::string> KdbClient::products() { return query_symbols("products"); }

std::vector<double> KdbClient::history(const std::string& symbol, int n) {
    return query_floats("hist[`$\"" + symbol + "\";" + std::to_string(n) + "]");
}

}  // namespace el
