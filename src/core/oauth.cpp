#include "core/oauth.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

#include <wil/resource.h>

#include "core/claude_process.h"

namespace aic {
namespace {

using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

std::string Base64Url(const unsigned char* data, size_t size) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const unsigned chunk = (static_cast<unsigned>(data[i]) << 16) | (i + 1 < size ? data[i + 1] << 8 : 0) |
                               (i + 2 < size ? data[i + 2] : 0);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        if (i + 1 < size) {
            out += alphabet[(chunk >> 6) & 63];
        }
        if (i + 2 < size) {
            out += alphabet[chunk & 63];
        }
    }
    return out;
}

std::string RandomToken(size_t bytes) {
    std::array<unsigned char, 64> random{};
    bytes = std::min(bytes, random.size());
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return {};
    }
    return Base64Url(random.data(), bytes);
}

std::string Sha256Url(const std::string& text) {
    std::array<unsigned char, 32> hash{};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),
               static_cast<ULONG>(text.size()), hash.data(), static_cast<ULONG>(hash.size()));
    return Base64Url(hash.data(), hash.size());
}

std::string PercentDecode(std::string_view text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(text.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += text[i] == '+' ? ' ' : text[i];
        }
    }
    return out;
}

// The value of `key` in the query of "GET /path?a=b&c=d HTTP/1.1".
std::string QueryValue(const std::string& request, std::string_view key) {
    const size_t start = request.find("GET ");
    if (start != 0) {
        return {};
    }
    const size_t end = request.find(' ', 4);
    const std::string target = request.substr(4, end == std::string::npos ? std::string::npos : end - 4);
    const size_t question = target.find('?');
    if (question == std::string::npos) {
        return {};
    }
    std::string_view rest = std::string_view(target).substr(question + 1);
    while (!rest.empty()) {
        const size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        if (const size_t eq = pair.find('='); eq != std::string_view::npos && pair.substr(0, eq) == key) {
            return PercentDecode(pair.substr(eq + 1));
        }
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
    }
    return {};
}

struct SocketGuard {
    SOCKET socket = INVALID_SOCKET;
    SocketGuard() = default;
    explicit SocketGuard(SOCKET s) : socket(s) {}
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
    ~SocketGuard() {
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
        }
    }
};

void Answer(SOCKET client, const std::string& html) {
    const std::string head = std::format(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: {}\r\n\r\n",
        html.size());
    const std::string all = head + html;
    send(client, all.data(), static_cast<int>(all.size()), 0);
}

std::string HtmlEscape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '&':
            out += "&amp;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

}  // namespace

HttpsReply HttpsRequest(const wchar_t* method, const wchar_t* host, const std::wstring& path, const std::wstring& headers,
                        const std::string& body, size_t cap) {
    HttpsReply reply;
    unique_hinternet session(WinHttpOpen(L"AIControls", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        reply.error = "WinHttpOpen failed";
        return reply;
    }
    WinHttpSetTimeouts(session.get(), 10000, 10000, 20000, 60000);
    unique_hinternet connection(WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    unique_hinternet request(connection ? WinHttpOpenRequest(connection.get(), method, path.c_str(), nullptr,
                                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
    if (!request) {
        reply.error = "WinHttpOpenRequest failed";
        return reply;
    }
    const DWORD size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), size, size, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        reply.error = std::format("request failed (error {})", GetLastError());
        return reply;
    }
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    reply.status = static_cast<int>(status);
    std::vector<char> chunk(16384);
    while (reply.body.size() < cap) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got) || got == 0) {
            break;
        }
        reply.body.append(chunk.data(), got);
    }
    return reply;
}

std::string UrlEncode(std::string_view text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

std::string FormBody(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) {
        body += (body.empty() ? "" : "&") + key + "=" + UrlEncode(value);
    }
    return body;
}

std::string OAuthPage(std::string_view message, bool ok) {
    return std::format(
        "<!doctype html><meta charset=utf-8><title>AIControls</title><body style=\"font:16px system-ui;"
        "background:#1c1d21;color:{};display:grid;place-items:center;height:100vh;margin:0\"><div>{}</div>",
        ok ? "#e6e6e6" : "#f06060", HtmlEscape(message));
}

BrowserEnd BrowserSignIn(const std::string& authorize, const std::string& path, std::string_view service,
                         const std::function<std::string(const OAuthGrant&)>& exchange, const std::atomic<bool>& cancel,
                         std::chrono::seconds wait, std::string& error) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = "WSAStartup failed";
        return BrowserEnd::Broken;
    }
    const auto cleanup = wil::scope_exit([] { WSACleanup(); });
    SocketGuard listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof(address);
    if (listener.socket == INVALID_SOCKET ||
        bind(listener.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener.socket, 4) != 0 || getsockname(listener.socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        error = std::format("Could not listen on 127.0.0.1 for {}'s answer", service);
        return BrowserEnd::Broken;
    }
    OAuthGrant grant;
    grant.redirect = std::format("http://127.0.0.1:{}{}", ntohs(address.sin_port), path);
    grant.verifier = RandomToken(48);
    const std::string state = RandomToken(24);
    if (state.empty() || grant.verifier.empty()) {
        error = "The system's random generator failed";
        return BrowserEnd::Broken;
    }
    const std::string url = authorize + (authorize.find('?') == std::string::npos ? "?" : "&") +
                            FormBody({{"redirect_uri", grant.redirect},
                                      {"code_challenge", Sha256Url(grant.verifier)},
                                      {"code_challenge_method", "S256"},
                                      {"state", state}});
    ShellExecuteW(nullptr, L"open", ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (!cancel && std::chrono::steady_clock::now() < deadline) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener.socket, &ready);
        const timeval tick{0, 500'000};
        if (select(0, &ready, nullptr, nullptr, &tick) <= 0) {
            continue;
        }
        SocketGuard client(accept(listener.socket, nullptr, nullptr));
        if (client.socket == INVALID_SOCKET) {
            continue;
        }
        const DWORD timeout = 2000;
        setsockopt(client.socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        std::array<char, 2048> buffer{};
        while (request.size() < 8192 && request.find("\r\n\r\n") == std::string::npos) {
            const int got = recv(client.socket, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (got <= 0) {
                break;
            }
            request.append(buffer.data(), got);
        }
        if (QueryValue(request, "state") != state) {  // a favicon, or someone else's request
            continue;
        }
        grant.code = QueryValue(request, "code");
        if (!grant.code.empty()) {
            Answer(client.socket, exchange(grant));
            return BrowserEnd::Answered;
        }
        if (const std::string denied = QueryValue(request, "error"); !denied.empty()) {
            const std::string more = QueryValue(request, "error_description");
            Answer(client.socket, OAuthPage(std::format("{}: {}", service, more.empty() ? denied : more), false));
            error = std::format("{} refused the sign-in: {}", service, more.empty() ? denied : more);
            return BrowserEnd::Refused;
        }
    }
    if (cancel) {
        error = "Sign-in cancelled";
        return BrowserEnd::Cancelled;
    }
    error = std::format("No answer from the browser in {} minutes: the sign-in to {} was not finished",
                        wait.count() / 60, service);
    return BrowserEnd::TimedOut;
}

}  // namespace aic
