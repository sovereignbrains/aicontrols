#include "core/loopback_server.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace aic {
namespace {

constexpr size_t kMaxHeader = 64 * 1024;
constexpr size_t kMaxBody = 16 * 1024 * 1024;

void EnsureWinsock() {
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!ready) {
        throw std::runtime_error("WSAStartup failed");
    }
}

std::string Lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

bool SendAll(SOCKET socket, std::string_view data) {
    while (!data.empty()) {
        const int sent = send(socket, data.data(), static_cast<int>(std::min<size_t>(data.size(), 1 << 20)), 0);
        if (sent <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<size_t>(sent));
    }
    return true;
}

bool Reply(SOCKET socket, int status, std::string_view reason, std::string_view body, bool keepAlive) {
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason) + "\r\n";
    if (!body.empty()) {
        head += "Content-Type: application/json\r\n";
    }
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    return SendAll(socket, head) && SendAll(socket, body);
}

}  // namespace

LoopbackServer::~LoopbackServer() {
    Stop();
}

uint16_t LoopbackServer::Start(std::string token, Handler handler) {
    Stop();
    EnsureWinsock();
    token_ = std::move(token);
    handler_ = std::move(handler);
    stopping_ = false;

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        throw std::runtime_error("socket failed");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(listener);
        throw std::runtime_error("cannot listen on the loopback");
    }
    listener_ = listener;
    acceptor_ = std::thread(&LoopbackServer::AcceptLoop, this);
    return ntohs(address.sin_port);
}

void LoopbackServer::Stop() {
    stopping_ = true;
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    std::list<Connection> connections;
    {
        std::lock_guard lock(mutex_);
        for (Connection& connection : connections_) {
            if (connection.socket != INVALID_SOCKET) {
                shutdown(connection.socket, SD_BOTH);  // its recv returns; the thread closes the socket itself
            }
        }
        connections.swap(connections_);
    }
    for (Connection& connection : connections) {
        if (connection.thread.joinable()) {
            connection.thread.join();
        }
    }
}

void LoopbackServer::Reap() {
    std::lock_guard lock(mutex_);
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (it->done) {
            it->thread.join();
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void LoopbackServer::AcceptLoop() {
    while (!stopping_) {
        const SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            return;  // the listener was closed
        }
        Reap();
        std::lock_guard lock(mutex_);
        if (stopping_) {
            closesocket(client);
            return;
        }
        Connection& connection = connections_.emplace_back();
        connection.socket = client;
        connection.thread = std::thread(&LoopbackServer::Serve, this, std::ref(connection));
    }
}

void LoopbackServer::Serve(Connection& connection) {
    const SOCKET socket = connection.socket;
    std::string buffer;
    std::vector<char> chunk(16384);  // on the heap: the analyzer's stack limit
    const auto fill = [&] {
        const int got = recv(socket, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (got <= 0) {
            return false;
        }
        buffer.append(chunk.data(), static_cast<size_t>(got));
        return true;
    };

    for (bool open = true; open;) {
        size_t headerEnd = std::string::npos;
        while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (buffer.size() > kMaxHeader || !fill()) {
                open = false;
                break;
            }
        }
        if (!open) {
            break;
        }
        const std::string_view head(buffer.data(), headerEnd);
        const size_t firstEnd = head.find("\r\n");
        const std::string_view requestLine = head.substr(0, firstEnd);
        const std::string method(requestLine.substr(0, requestLine.find(' ')));

        size_t contentLength = 0;
        bool hasLength = false;
        bool authorized = false;
        bool keepAlive = true;
        for (size_t at = firstEnd == std::string_view::npos ? head.size() : firstEnd + 2; at < head.size();) {
            size_t end = head.find("\r\n", at);
            if (end == std::string_view::npos) {
                end = head.size();
            }
            const std::string_view line = head.substr(at, end - at);
            at = end + 2;
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                continue;
            }
            const std::string name = Lower(line.substr(0, colon));
            const std::string_view value = Trim(line.substr(colon + 1));
            if (name == "content-length") {
                hasLength = true;
                contentLength = static_cast<size_t>(std::strtoull(std::string(value).c_str(), nullptr, 10));
            } else if (name == "authorization") {
                authorized = value == "Bearer " + token_;
            } else if (name == "connection") {
                keepAlive = Lower(value) != "close";
            }
        }
        if (contentLength > kMaxBody) {
            Reply(socket, 413, "Payload Too Large", {}, false);
            break;
        }
        buffer.erase(0, headerEnd + 4);
        while (buffer.size() < contentLength) {
            if (!fill()) {
                open = false;
                break;
            }
        }
        if (!open) {
            break;
        }
        const std::string body = buffer.substr(0, contentLength);
        buffer.erase(0, contentLength);

        if (!authorized) {
            open = Reply(socket, 401, "Unauthorized", {}, keepAlive) && keepAlive;
        } else if (method != "POST") {
            open = Reply(socket, 405, "Method Not Allowed", {}, keepAlive) && keepAlive;
        } else if (!hasLength) {
            open = Reply(socket, 411, "Length Required", {}, false) && false;
        } else {
            const std::string answer = handler_(body);
            open = (answer.empty() ? Reply(socket, 202, "Accepted", {}, keepAlive)
                                   : Reply(socket, 200, "OK", answer, keepAlive)) &&
                   keepAlive;
        }
    }

    {
        std::lock_guard lock(mutex_);
        connection.socket = INVALID_SOCKET;
        closesocket(socket);
    }
    connection.done = true;
}

}  // namespace aic
