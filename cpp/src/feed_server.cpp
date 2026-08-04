#include "execution/feed_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "execution/market_store.hpp"
#include "json.hpp"

using json = nlohmann::json;

namespace el {

namespace {

// Poll interval on both the accept and read loops, so stop() is honoured within
// this long rather than blocking until the next byte arrives.
constexpr int kPollMs = 200;

bool send_all(int fd, const std::string& s) {
    std::size_t sent = 0;
    while (sent < s.size()) {
        const ssize_t n = ::send(fd, s.data() + sent, s.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

FeedServer::~FeedServer() { stop(); }

bool FeedServer::start() {
    if (running_) return true;

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { err_ = "socket() failed"; return false; }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only, never exposed
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err_ = "bind() failed on port " + std::to_string(port_) + " (already running?)";
        ::close(fd);
        return false;
    }
    if (::listen(fd, 1) != 0) {
        err_ = "listen() failed";
        ::close(fd);
        return false;
    }

    listen_fd_ = fd;
    running_ = true;
    err_.clear();
    thread_ = std::thread([this] { run(); });
    return true;
}

void FeedServer::stop() {
    if (!running_ && listen_fd_ < 0) return;
    running_ = false;
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);  // unblock accept()
    if (thread_.joinable()) thread_.join();
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    store_.set_feed_attached(false);
}

void FeedServer::run() {
    while (running_) {
        pollfd pfd{listen_fd_, POLLIN, 0};
        const int r = ::poll(&pfd, 1, kPollMs);
        if (r <= 0) continue;

        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) continue;

        int one = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        store_.set_feed_attached(true);
        serve(client);
        store_.set_feed_attached(false);
        ::close(client);
        // Loop back to accept: the feedhandler restarting is routine, and the
        // terminal should pick it straight back up without a restart of its own.
    }
}

void FeedServer::serve(int client_fd) {
    std::string buf;
    char chunk[16384];

    while (running_) {
        pollfd pfd{client_fd, POLLIN, 0};
        const int r = ::poll(&pfd, 1, kPollMs);
        if (r < 0) return;
        if (r == 0) continue;

        const ssize_t n = ::recv(client_fd, chunk, sizeof(chunk), 0);
        if (n <= 0) return;  // publisher disconnected
        buf.append(chunk, static_cast<std::size_t>(n));

        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = buf.find('\n', start);
            if (nl == std::string::npos) break;
            handle_line(buf.substr(start, nl - start), client_fd);
            start = nl + 1;
        }
        buf.erase(0, start);
    }
}

void FeedServer::handle_line(const std::string& line, int client_fd) {
    if (line.empty()) return;

    // A malformed batch must never take the terminal down: drop it and keep
    // reading. The feed is untrusted input in exactly the same way a socket
    // from any other process is.
    json msg = json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (msg.is_discarded() || !msg.is_object()) return;

    const std::string m = msg.value("m", "");

    if (m == "quotes") {
        const auto rows = msg.find("rows");
        if (rows == msg.end() || !rows->is_array()) return;
        for (const auto& row : *rows) {
            if (!row.is_array() || row.size() < 6) continue;
            Quote q;
            q.symbol = row[0].get<std::string>();
            q.bid = row[1].get<double>();
            q.ask = row[2].get<double>();
            q.bsize = row[3].get<double>();
            q.asize = row[4].get<double>();
            q.ts_ns = row[5].get<TimestampNs>();
            store_.on_quote(q);
        }
        return;
    }

    if (m == "trades") {
        const auto rows = msg.find("rows");
        if (rows == msg.end() || !rows->is_array()) return;
        for (const auto& row : *rows) {
            if (!row.is_array() || row.size() < 3) continue;
            store_.on_trade(row[0].get<std::string>(), row[1].get<double>());
        }
        return;
    }

    if (m == "products" || m == "universe") {
        const auto syms = msg.find("syms");
        if (syms == msg.end() || !syms->is_array()) return;
        std::vector<std::string> out;
        out.reserve(syms->size());
        for (const auto& s : *syms)
            if (s.is_string()) out.push_back(s.get<std::string>());
        if (m == "products") store_.set_products(std::move(out));
        else store_.set_universe(std::move(out));
        return;
    }

    if (m == "poll") {
        json reply;
        reply["m"] = "requested";
        reply["syms"] = store_.take_requested();
        send_all(client_fd, reply.dump() + "\n");
        return;
    }
}

}  // namespace el
