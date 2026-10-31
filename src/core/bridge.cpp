#include "core/bridge.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <wil/resource.h>

#include "core/claude_process.h"
#include "core/secrets.h"

namespace aic {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

constexpr uint16_t kSingBoxPort = 2080;
constexpr size_t kMaxHeader = 16 * 1024;

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

bool ReceiveExact(SOCKET socket, char* data, size_t size) {
    while (size > 0) {
        const int got = recv(socket, data, static_cast<int>(size), 0);
        if (got <= 0) {
            return false;
        }
        data += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

void SetReceiveTimeout(SOCKET socket, DWORD ms) {
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
}

// A TCP connection to host:port, each of its addresses given `timeout`; INVALID_SOCKET when none answered.
// Non-blocking until connected: a refused or silent address costs the timeout, not Windows' ~20 s of retries.
SOCKET Connect(const std::string& host, uint16_t port, std::chrono::milliseconds timeout) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &found) != 0) {
        return INVALID_SOCKET;
    }
    const std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> list(found, &freeaddrinfo);
    const long long us = std::chrono::duration_cast<std::chrono::microseconds>(timeout).count();
    for (const addrinfo* address = list.get(); address != nullptr; address = address->ai_next) {
        const SOCKET s = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (s == INVALID_SOCKET) {
            continue;
        }
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        connect(s, address->ai_addr, static_cast<int>(address->ai_addrlen));
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(s, &writable);
        timeval wait{static_cast<long>(us / 1'000'000), static_cast<long>(us % 1'000'000)};
        if (select(0, nullptr, &writable, nullptr, &wait) > 0 && FD_ISSET(s, &writable)) {
            nonBlocking = 0;
            ioctlsocket(s, FIONBIO, &nonBlocking);
            const BOOL noDelay = TRUE;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            return s;
        }
        closesocket(s);
    }
    return INVALID_SOCKET;
}

// host:port through the SOCKS5 proxy on the loopback's `proxy` port, which resolves the name itself.
SOCKET ConnectThroughSocks(uint16_t proxy, const std::string& host, uint16_t port) {
    if (host.empty() || host.size() > 255) {
        return INVALID_SOCKET;
    }
    const SOCKET s = Connect("127.0.0.1", proxy, 300ms);
    if (s == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }
    SetReceiveTimeout(s, 10000);
    std::array<char, 4> reply{};
    bool ok = SendAll(s, std::string_view("\x05\x01\x00", 3)) && ReceiveExact(s, reply.data(), 2) && reply[0] == 5 &&
              reply[1] == 0;
    if (ok) {
        std::string request("\x05\x01\x00\x03", 4);
        request += static_cast<char>(host.size());
        request += host;
        request += static_cast<char>(port >> 8);
        request += static_cast<char>(port & 0xff);
        ok = SendAll(s, request) && ReceiveExact(s, reply.data(), 4) && reply[0] == 5 && reply[1] == 0;
    }
    if (ok) {  // the address it connected from, which nobody needs: IPv4, a name or IPv6, then a port
        size_t rest = 2;
        if (reply[3] == 1) {
            rest += 4;
        } else if (reply[3] == 4) {
            rest += 16;
        } else if (char length = 0; reply[3] == 3 && ReceiveExact(s, &length, 1)) {
            rest += static_cast<unsigned char>(length);
        } else {
            ok = false;
        }
        std::array<char, 258> bound{};
        ok = ok && ReceiveExact(s, bound.data(), rest);
    }
    if (!ok) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    SetReceiveTimeout(s, 0);
    return s;
}

// Where api.anthropic.com's edge thinks this computer is. Reaching the API is not being allowed to use it: from a
// Russian line the handshake works and every request then comes back 403 "forbidden", a country block. Cloudflare
// itself answers /cdn-cgi/trace on that host with loc=<country>. A failed handshake (DPI) or a timeout: Unknown;
// no loc line at all: the handshake worked, Allowed.
enum class Direct { Allowed, Blocked, Unknown };

Direct ProbeDirect() {
    unique_hinternet session(
        WinHttpOpen(L"AIControls", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        return Direct::Unknown;
    }
    WinHttpSetTimeouts(session.get(), 2000, 2000, 2000, 2000);
    unique_hinternet connection(WinHttpConnect(session.get(), L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    unique_hinternet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", L"/cdn-cgi/trace", nullptr,
                                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
    if (!request ||
        !WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        return Direct::Unknown;
    }
    std::string body;
    std::array<char, 2048> chunk{};
    DWORD got = 0;
    while (body.size() < 8192 &&
           WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got) && got > 0) {
        body.append(chunk.data(), got);
    }
    const size_t line = body.starts_with("loc=") ? 0 : body.find("\nloc=");
    if (line == std::string::npos) {
        return Direct::Allowed;
    }
    const size_t from = body.find('=', line) + 1;
    const std::string country = body.substr(from, body.find_first_of("\r\n", from) - from);
    static constexpr std::array<std::string_view, 9> kBlocked{"RU", "BY", "CN", "HK", "MO", "IR", "KP", "SY", "CU"};
    return std::ranges::find(kBlocked, country) != kBlocked.end() ? Direct::Blocked : Direct::Allowed;
}

// A free port on the loopback for ssh's -D: the system picks it, and it is let go for ssh to take.
uint16_t FreePort() {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    const bool ok = bind(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 &&
                    getsockname(s, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    closesocket(s);
    return ok ? ntohs(address.sin_port) : 0;
}

// The servers the user stored a password for ("ssh user@host[:port]"), as ssh takes them; anything that could be
// read as an option or break the command line is left out.
std::vector<std::string> TunnelServers() {
    SecretStore store(SecretStore::DefaultPath());
    store.Load();
    std::vector<std::string> servers;
    for (const std::string& name : store.Names()) {
        if (name.starts_with("ssh ") && name.size() > 4 && name[4] != '-' &&
            name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@:", 4) ==
                std::string::npos) {
            servers.push_back(name.substr(4));
        }
    }
    return servers;
}

struct Tunnel {
    wil::unique_handle job;  // ssh dies with it, and so with this program
    wil::unique_handle process;
    uint16_t port = 0;
};

bool Alive(const Tunnel& tunnel) {
    return tunnel.process && WaitForSingleObject(tunnel.process.get(), 0) == WAIT_TIMEOUT;
}

// ssh.exe -D for `server`, waited for until its SOCKS port answers: ssh opens its forwards only once logged in.
// False when it exited or did not get there in time; nothing of it is left running then.
bool StartTunnel(Tunnel& tunnel, const std::string& server) {
    std::string host = server;
    std::string port;
    if (const size_t colon = server.rfind(':'); colon != std::string::npos) {
        host = server.substr(0, colon);
        port = server.substr(colon + 1);
    }
    const uint16_t socks = FreePort();
    if (socks == 0) {
        return false;
    }
    std::wstring line = L"ssh.exe -n -N -D 127.0.0.1:" + std::to_wstring(socks) +
                        L" -o ExitOnForwardFailure=yes -o ConnectTimeout=10 -o ServerAliveInterval=15"
                        L" -o ServerAliveCountMax=3 -o StrictHostKeyChecking=yes -o NumberOfPasswordPrompts=1";
    if (!port.empty()) {
        line += L" -p " + ToWide(port);
    }
    line += L" " + ToWide(host);
    std::vector<wchar_t> environment = MakeEnvironmentBlock({{L"SSH_ASKPASS", OwnExe()},
                                                             {L"SSH_ASKPASS_REQUIRE", L"force"},
                                                             {L"AIC_ASKPASS", ToWide("ssh " + server)}});
    wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr,
                        &startup, &info)) {
        return false;
    }
    wil::unique_handle process(info.hProcess);
    const wil::unique_handle thread(info.hThread);
    AssignProcessToJobObject(job.get(), process.get());
    ResumeThread(thread.get());
    for (const auto deadline = Clock::now() + 20s;
         Clock::now() < deadline && WaitForSingleObject(process.get(), 150) == WAIT_TIMEOUT;) {
        if (const SOCKET probe = Connect("127.0.0.1", socks, 200ms); probe != INVALID_SOCKET) {
            closesocket(probe);
            tunnel.job = std::move(job);
            tunnel.process = std::move(process);
            tunnel.port = socks;
            return true;
        }
    }
    return false;
}

// What the connections share. Never destroyed: their threads may outlive any static destructor.
struct State {
    std::mutex directMutex;  // held through a probe: the connections that come meanwhile wait for its answer
    Direct direct = Direct::Unknown;
    std::optional<Clock::time_point> directAt;
    std::mutex tunnelMutex;  // held while a tunnel starts, for the same reason
    Tunnel tunnel;
    std::optional<Clock::time_point> tunnelFailed;
};

State& Shared() {
    static State* const state = new State;
    return *state;
}

std::atomic<bool> g_withoutDirect = false;

// The answer changes when the VPN comes or goes, so it lives a few seconds: the CLI keeps its connections and
// opens new ones rarely, and the probe takes ~0.3 s.
Direct DirectWay() {
    if (g_withoutDirect) {
        return Direct::Blocked;
    }
    State& state = Shared();
    std::lock_guard lock(state.directMutex);
    if (!state.directAt || Clock::now() - *state.directAt > 5s) {
        state.direct = ProbeDirect();
        state.directAt = Clock::now();
    }
    return state.direct;
}

void ForgetDirect() {
    State& state = Shared();
    std::lock_guard lock(state.directMutex);
    state.directAt.reset();
}

// The running tunnel's SOCKS port, a new one's, or 0. After every server failed, none is tried for half a minute:
// each try can take 20 s, and the connections wait for it.
uint16_t TunnelPort() {
    State& state = Shared();
    std::lock_guard lock(state.tunnelMutex);
    if (Alive(state.tunnel)) {
        return state.tunnel.port;
    }
    if (state.tunnelFailed && Clock::now() - *state.tunnelFailed < 30s) {
        return 0;
    }
    for (const std::string& server : TunnelServers()) {
        if (StartTunnel(state.tunnel, server)) {
            state.tunnelFailed.reset();
            return state.tunnel.port;
        }
    }
    state.tunnelFailed = Clock::now();
    return 0;
}

// The first way out that opens (bridge.h); `direct` tells which it was.
SOCKET OpenUpstream(const std::string& host, uint16_t port, bool& direct) {
    const Direct way = DirectWay();
    direct = true;
    if (way == Direct::Allowed) {
        if (const SOCKET s = Connect(host, port, 5s); s != INVALID_SOCKET) {
            return s;
        }
        ForgetDirect();
    }
    direct = false;
    if (const SOCKET s = ConnectThroughSocks(kSingBoxPort, host, port); s != INVALID_SOCKET) {
        return s;
    }
    if (const uint16_t tunnel = TunnelPort(); tunnel != 0) {
        if (const SOCKET s = ConnectThroughSocks(tunnel, host, port); s != INVALID_SOCKET) {
            return s;
        }
    }
    // A probe that timed out is no country block: the plain line still gets its try, last.
    if (way == Direct::Unknown) {
        direct = true;
        return Connect(host, port, 5s);
    }
    return INVALID_SOCKET;
}

// Copies both ways until either side closes. True when the upstream broke rather than closed: the first sign the
// way out has changed (the VPN went off under a direct connection).
bool Relay(SOCKET client, SOCKET upstream) {
    std::vector<char> buffer(64 * 1024);
    std::array<WSAPOLLFD, 2> sockets{};
    sockets[0].fd = client;
    sockets[0].events = POLLRDNORM;
    sockets[1].fd = upstream;
    sockets[1].events = POLLRDNORM;
    for (;;) {
        if (WSAPoll(sockets.data(), static_cast<ULONG>(sockets.size()), -1) <= 0) {
            return false;
        }
        for (size_t i = 0; i < sockets.size(); ++i) {
            if (sockets[i].revents == 0) {
                continue;
            }
            const int got = recv(sockets[i].fd, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (got <= 0) {
                return i == 1 && got < 0;
            }
            if (!SendAll(sockets[1 - i].fd, std::string_view(buffer.data(), static_cast<size_t>(got)))) {
                return i == 0;
            }
        }
    }
}

// An IPv4 address, network order, that is no public one (LocalTarget).
bool LocalV4(const uint8_t* a) {
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || a[0] >= 224 || (a[0] == 100 && (a[1] & 0xc0) == 64) ||
           (a[0] == 169 && a[1] == 254) || (a[0] == 172 && (a[1] & 0xf0) == 16) || (a[0] == 192 && a[1] == 168);
}

// One CLI connection: "CONNECT host:443 HTTP/1.1", the only request a CLI sends a proxy for an https:// API, then
// the bytes both ways. Other ports and local targets are refused: this is no open proxy for whatever runs on the
// computer, and no way into a network - the local one, or the SSH server's.
void Serve(SOCKET client) {
    SetReceiveTimeout(client, 15000);
    std::string buffer;
    std::array<char, 4096> chunk{};
    size_t end = std::string::npos;
    while ((end = buffer.find("\r\n\r\n")) == std::string::npos) {
        const int got = buffer.size() > kMaxHeader ? 0 : recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
        if (got <= 0) {
            closesocket(client);
            return;
        }
        buffer.append(chunk.data(), static_cast<size_t>(got));
    }
    const std::string_view line = std::string_view(buffer).substr(0, buffer.find("\r\n"));
    const size_t space = line.find(' ');
    const std::string_view target = space == std::string_view::npos
                                        ? std::string_view()
                                        : line.substr(space + 1, line.find(' ', space + 1) - space - 1);
    const size_t colon = target.rfind(':');
    std::string host(target.substr(0, colon == std::string_view::npos ? 0 : colon));
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    uint16_t port = 0;
    if (colon != std::string_view::npos) {
        std::from_chars(target.data() + colon + 1, target.data() + target.size(), port);
    }
    if (line.substr(0, space) != "CONNECT" || host.empty() || port != 443) {
        SendAll(client, "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        closesocket(client);
        return;
    }
    if (LocalTarget(host)) {  // at once: trying the ways out first would cost seconds, and could start a tunnel
        SendAll(client, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        closesocket(client);
        return;
    }
    bool direct = false;
    const SOCKET upstream = OpenUpstream(host, port, direct);
    if (upstream == INVALID_SOCKET) {
        SendAll(client, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        closesocket(client);
        return;
    }
    SetReceiveTimeout(client, 0);
    if (SendAll(client, "HTTP/1.1 200 Connection Established\r\n\r\n") &&
        SendAll(upstream, std::string_view(buffer).substr(end + 4)) && Relay(client, upstream) && direct) {
        ForgetDirect();
    }
    closesocket(upstream);
    closesocket(client);
}

uint16_t Listen() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return 0;
    }
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return 0;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(address);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, SOMAXCONN) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        closesocket(listener);
        return 0;
    }
    std::thread([listener] {
        for (;;) {
            const SOCKET client = accept(listener, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                std::thread(Serve, client).detach();
            } else if (const int error = WSAGetLastError(); error == WSAENOTSOCK || error == WSAEINVAL) {
                return;
            } else {
                std::this_thread::sleep_for(100ms);
            }
        }
    }).detach();
    return ntohs(address.sin_port);
}

}  // namespace

std::wstring BridgeUrl() {
    static const std::wstring url = [] {
        const uint16_t port = Listen();
        return port == 0 ? std::wstring() : L"http://127.0.0.1:" + std::to_wstring(port);
    }();
    return url;
}

void BridgeWithoutDirect() {
    g_withoutDirect = true;
}

bool LocalTarget(std::string_view host) {
    std::string name(host.substr(0, host.find('%')));  // fe80::1%3: the zone is no part of the address
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    while (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    in_addr v4{};
    if (InetPtonA(AF_INET, name.c_str(), &v4) == 1) {
        // 010.0.0.1 is 8.0.0.1 to some resolvers: only the usual spelling is the address it looks like.
        std::array<char, INET_ADDRSTRLEN> usual{};
        return LocalV4(reinterpret_cast<const uint8_t*>(&v4)) ||
               InetNtopA(AF_INET, &v4, usual.data(), usual.size()) == nullptr || name != usual.data();
    }
    in6_addr v6{};
    if (InetPtonA(AF_INET6, name.c_str(), &v6) == 1) {
        const uint8_t* b = v6.u.Byte;
        const auto zero = [](uint8_t x) { return x == 0; };
        // ::a.b.c.d (and so :: and ::1), ::ffff:a.b.c.d, NAT64's 64:ff9b::a.b.c.d
        const bool v4Inside =
            (std::all_of(b, b + 10, zero) && ((b[10] == 0 && b[11] == 0) || (b[10] == 0xff && b[11] == 0xff))) ||
            (b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && std::all_of(b + 4, b + 12, zero));
        // fc00::/7 unique local, ff00::/8 multicast, fe80::/10 link-local and fec0::/10 site-local
        return (v4Inside && LocalV4(b + 12)) || (b[0] & 0xfe) == 0xfc || b[0] == 0xff ||
               (b[0] == 0xfe && (b[1] & 0x80) != 0);
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return true;  // one label: localhost, the router, a LAN's own names; 2130706433
    }
    // An address in a resolver's loose forms ends in a number: 127.1, 0x7f.1, 1.0x2
    const std::string_view last = std::string_view(name).substr(dot + 1);
    if (last.empty() || last.starts_with("0x") ||
        std::ranges::all_of(last, [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return true;
    }
    constexpr std::array<std::string_view, 7> kLocal = {".localhost", ".local", ".lan", ".home", ".home.arpa",
                                                        ".internal", ".intranet"};
    const std::string dotted = "." + name;
    return std::ranges::any_of(kLocal, [&](std::string_view suffix) { return dotted.ends_with(suffix); });
}

}  // namespace aic
