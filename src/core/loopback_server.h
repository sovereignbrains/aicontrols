#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>

namespace aic {

// A minimal HTTP/1.1 server on 127.0.0.1 for the one thing that needs it: a CLI that reaches our tools as an
// MCP server over HTTP (Grok has no in-process channel like Claude's). POST with a bearer token, a body with a
// Content-Length, one JSON reply; nothing else is served.
class LoopbackServer {
public:
    // The reply's JSON text; empty: 202 Accepted without a body (a notification).
    // Called on a connection's thread; it may block until the answer is ready.
    using Handler = std::function<std::string(const std::string& body)>;

    LoopbackServer() = default;
    ~LoopbackServer();
    LoopbackServer(const LoopbackServer&) = delete;
    LoopbackServer& operator=(const LoopbackServer&) = delete;

    // Listens on a free port and returns it. Throws std::runtime_error. Requests without
    // "Authorization: Bearer <token>" get 401.
    uint16_t Start(std::string token, Handler handler);
    // Closes the connections and joins their threads; a handler still blocked must be woken first.
    void Stop();

private:
    struct Connection {
        SOCKET socket = INVALID_SOCKET;  // guarded by mutex_
        std::thread thread;
        std::atomic<bool> done = false;
    };

    void AcceptLoop();
    void Serve(Connection& connection);
    void Reap();

    SOCKET listener_ = INVALID_SOCKET;
    std::thread acceptor_;
    std::string token_;
    Handler handler_;
    std::atomic<bool> stopping_ = false;
    std::mutex mutex_;
    std::list<Connection> connections_;  // guarded by mutex_
};

}  // namespace aic
