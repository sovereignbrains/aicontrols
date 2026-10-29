#include "core/gdrive.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

#include <wil/resource.h>

namespace aic {
namespace {

using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

constexpr size_t kFetchCap = 4u << 20;  // bytes taken from Google for one read; the rest is not fetched
constexpr size_t kReadBytes = 60'000;   // what one drive_read hands the model at most
constexpr int kListDefault = 20;
constexpr int kListMax = 100;
constexpr auto kSignInWait = std::chrono::minutes(3);
constexpr const char* kScope = "https://www.googleapis.com/auth/drive.readonly";
constexpr const char* kGoogleDocMime = "application/vnd.google-apps.";

ToolResult Fail(std::string text) {
    return ToolResult{std::move(text), true};
}

struct Reply {
    int status = 0;  // 0: no answer, `error` says why
    std::string body;
    std::string error;
};

// One HTTPS request; the body is cut at kFetchCap.
Reply Http(const wchar_t* method, const wchar_t* host, const std::wstring& path, const std::wstring& headers,
           const std::string& body = {}) {
    Reply reply;
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
    while (reply.body.size() < kFetchCap) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got) || got == 0) {
            break;
        }
        reply.body.append(chunk.data(), got);
    }
    return reply;
}

// Google's complaint in a sentence: {"error":{"message":..}} from the API, {"error":"..","error_description":..}
// from the token endpoint.
std::string ApiError(const Reply& reply) {
    if (reply.status == 0) {
        return reply.error;
    }
    std::string text = std::format("HTTP {}", reply.status);
    const Json json = Json::parse(reply.body, nullptr, false);
    if (json.is_object()) {
        const Json& error = JsonAt(json, "error");
        if (error.is_string()) {
            text += ": " + error.get<std::string>();
            if (const std::string more = JsonStr(json, "error_description"); !more.empty()) {
                text += " (" + more + ")";
            }
        } else if (error.is_object()) {
            if (const std::string message = JsonStr(error, "message"); !message.empty()) {
                text += ": " + message;
            }
        }
    }
    return text;
}

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

// The value of `key` in the query of "GET /?a=b&c=d HTTP/1.1".
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

constexpr const char* kDonePage =
    "<!doctype html><meta charset=utf-8><title>AIControls</title><body style=\"font:16px system-ui;background:#1c1d21;"
    "color:#e6e6e6;display:grid;place-items:center;height:100vh;margin:0\"><div>Google Диск подключён. "
    "Эту вкладку можно закрыть.</div>";

std::string FormBody(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string body;
    for (const auto& [key, value] : fields) {
        body += (body.empty() ? "" : "&") + key + "=" + GoogleDrive::UrlEncode(value);
    }
    return body;
}

constexpr const wchar_t* kFormHeaders = L"Content-Type: application/x-www-form-urlencoded\r\n";

// Where a UTF-8 text may be cut at `pos`: back to the start of the character.
size_t CharStart(const std::string& text, size_t pos) {
    while (pos > 0 && pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

}  // namespace

std::string GoogleDrive::UrlEncode(std::string_view text) {
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

std::string GoogleDrive::ExportType(std::string_view driveType) {
    if (driveType == "application/vnd.google-apps.document" || driveType == "application/vnd.google-apps.presentation") {
        return "text/plain";
    }
    if (driveType == "application/vnd.google-apps.spreadsheet") {
        return "text/csv";
    }
    return {};
}

bool GoogleDrive::TextFile(std::string_view mimeType, std::string_view name) {
    if (mimeType.starts_with("text/")) {
        return true;
    }
    for (const char* part : {"json", "xml", "yaml", "javascript", "csv", "x-sh", "toml", "markdown"}) {
        if (mimeType.find(part) != std::string_view::npos) {
            return true;
        }
    }
    const size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string extension(name.substr(dot + 1));
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* known : {"txt", "md", "csv", "tsv", "json", "xml", "yml", "yaml", "toml", "ini", "log", "html", "css",
                              "js", "ts", "py", "cpp", "h", "c", "cs", "java", "go", "rs", "sh", "ps1", "sql", "tex"}) {
        if (extension == known) {
            return true;
        }
    }
    return false;
}

std::string GoogleDrive::Query(const Json& input) {
    const auto escape = [](const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '\\' || c == '\'') {
                out += '\\';
            }
            out += c;
        }
        return out;
    };
    std::string query = "trashed = false";
    if (const std::string text = JsonStr(input, "text"); !text.empty()) {
        query += " and fullText contains '" + escape(text) + "'";
    }
    if (const std::string folder = JsonStr(input, "folder"); !folder.empty()) {
        query += " and '" + escape(folder) + "' in parents";
    }
    if (const std::string raw = JsonStr(input, "q"); !raw.empty()) {
        query += " and (" + raw + ")";
    }
    return query;
}

std::string GoogleDrive::SignIn(const std::string& clientId, const std::string& clientSecret,
                                const std::atomic<bool>& cancel, std::string& error) const {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = "WSAStartup failed";
        return {};
    }
    SocketGuard listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    int length = sizeof(address);
    if (listener.socket == INVALID_SOCKET ||
        bind(listener.socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener.socket, 4) != 0 || getsockname(listener.socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        error = "Could not listen on 127.0.0.1 for Google's answer";
        return {};
    }
    const std::string redirect = std::format("http://127.0.0.1:{}", ntohs(address.sin_port));
    const std::string state = RandomToken(24);
    const std::string verifier = RandomToken(48);
    if (state.empty() || verifier.empty()) {
        error = "The system's random generator failed";
        return {};
    }
    const std::string url =
        "https://accounts.google.com/o/oauth2/v2/auth?" +
        FormBody({{"client_id", clientId},
                  {"redirect_uri", redirect},
                  {"response_type", "code"},
                  {"scope", kScope},
                  {"code_challenge", Sha256Url(verifier)},
                  {"code_challenge_method", "S256"},
                  {"state", state},
                  {"access_type", "offline"},
                  {"prompt", "consent"}});
    ShellExecuteW(nullptr, L"open", ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    std::string code;
    std::string denied;
    const auto deadline = std::chrono::steady_clock::now() + kSignInWait;
    while (code.empty() && denied.empty() && !cancel && std::chrono::steady_clock::now() < deadline) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(listener.socket, &ready);
        const timeval wait{0, 500'000};
        if (select(0, &ready, nullptr, nullptr, &wait) <= 0) {
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
        code = QueryValue(request, "code");
        denied = QueryValue(request, "error");
        if (!code.empty()) {
            Answer(client.socket, kDonePage);
        } else if (!denied.empty()) {
            Answer(client.socket, "<!doctype html><meta charset=utf-8><body>Google: " + denied);
        }
    }
    if (code.empty()) {
        error = !denied.empty() ? "Google refused the sign-in: " + denied
                : cancel        ? "Sign-in cancelled"
                                : "No answer from the browser in 3 minutes: the user did not finish signing in to Google";
        return {};
    }

    const Reply reply = Http(L"POST", L"oauth2.googleapis.com", L"/token", kFormHeaders,
                             FormBody({{"client_id", clientId},
                                       {"client_secret", clientSecret},
                                       {"code", code},
                                       {"code_verifier", verifier},
                                       {"redirect_uri", redirect},
                                       {"grant_type", "authorization_code"}}));
    const Json json = Json::parse(reply.body, nullptr, false);
    const std::string refresh = reply.status == 200 ? JsonStr(json, "refresh_token") : std::string();
    if (refresh.empty()) {
        error = "Google's token request failed: " + ApiError(reply);
        return {};
    }
    return refresh;
}

std::string GoogleDrive::AccessToken(const std::atomic<bool>& cancel, std::string& error) const {
    static std::mutex mutex;  // two calls must not open two sign-in windows
    std::lock_guard lock(mutex);

    const auto clientId = secrets_.Get("GDRIVE_CLIENT_ID");
    const auto clientSecret = secrets_.Get("GDRIVE_CLIENT_SECRET");
    if (!clientId || !clientSecret || clientId->empty() || clientSecret->empty()) {
        error =
            "Google Drive is not set up: the user has to store the secrets GDRIVE_CLIENT_ID and GDRIVE_CLIENT_SECRET "
            "(an OAuth client of type Desktop app from Google Cloud Console) in the client's secrets panel.";
        return {};
    }
    // `invalid`: Google no longer honours the refresh token.
    const auto refresh = [&](const std::string& token, bool& invalid) -> std::string {
        const Reply reply = Http(L"POST", L"oauth2.googleapis.com", L"/token", kFormHeaders,
                                 FormBody({{"client_id", *clientId},
                                           {"client_secret", *clientSecret},
                                           {"refresh_token", token},
                                           {"grant_type", "refresh_token"}}));
        const Json json = Json::parse(reply.body, nullptr, false);
        invalid = reply.status == 400 && JsonStr(json, "error") == "invalid_grant";
        if (reply.status == 200 && !JsonStr(json, "access_token").empty()) {
            return JsonStr(json, "access_token");
        }
        error = "Google's token request failed: " + ApiError(reply);
        return {};
    };

    bool invalid = false;
    if (const auto stored = secrets_.Get("GDRIVE_REFRESH_TOKEN"); stored && !stored->empty()) {
        if (std::string token = refresh(*stored, invalid); !token.empty()) {
            return token;
        }
        if (!invalid) {
            return {};
        }
        secrets_.Remove("GDRIVE_REFRESH_TOKEN");
        secrets_.Save();
        error.clear();
    }
    const std::string fresh = SignIn(*clientId, *clientSecret, cancel, error);
    if (fresh.empty()) {
        return {};
    }
    secrets_.Set("GDRIVE_REFRESH_TOKEN", fresh);
    secrets_.Save();
    return refresh(fresh, invalid);
}

ToolResult GoogleDrive::Search(const Json& input, const std::atomic<bool>& cancel) const {
    std::string error;
    const std::string token = AccessToken(cancel, error);
    if (token.empty()) {
        return Fail(error);
    }
    const Json& limitValue = JsonAt(input, "limit");
    const int limit = std::clamp(limitValue.is_number_integer() ? limitValue.get<int>() : kListDefault, 1, kListMax);
    std::string path = "/drive/v3/files?q=" + UrlEncode(Query(input)) + "&pageSize=" + std::to_string(limit) +
                       "&fields=" + UrlEncode("files(id,name,mimeType,modifiedTime,size)") +
                       "&supportsAllDrives=true&includeItemsFromAllDrives=true&corpora=allDrives";
    if (JsonStr(input, "text").empty()) {
        path += "&orderBy=" + UrlEncode("modifiedTime desc");
    }
    const Reply reply = Http(L"GET", L"www.googleapis.com", ToWide(path), ToWide("Authorization: Bearer " + token + "\r\n"));
    if (reply.status != 200) {
        return Fail("Drive search failed: " + ApiError(reply));
    }
    const Json json = Json::parse(reply.body, nullptr, false);
    const Json& files = JsonAt(json, "files");
    if (!files.is_array() || files.empty()) {
        return {"No files found.", false};
    }
    std::string text = "name | id | type | modified | bytes\n";
    for (const Json& file : files) {
        const Json& size = JsonAt(file, "size");
        text += std::format("{} | {} | {} | {} | {}\n", JsonStr(file, "name"), JsonStr(file, "id"),
                            JsonStr(file, "mimeType"), JsonStr(file, "modifiedTime"),
                            size.is_string() ? size.get<std::string>() : std::string("-"));
    }
    return {std::move(text), false};
}

ToolResult GoogleDrive::Read(const Json& input, const std::atomic<bool>& cancel) const {
    const std::string id = JsonStr(input, "id");
    if (id.empty()) {
        return Fail("id is required (from drive_search)");
    }
    std::string error;
    const std::string token = AccessToken(cancel, error);
    if (token.empty()) {
        return Fail(error);
    }
    const std::wstring auth = ToWide("Authorization: Bearer " + token + "\r\n");
    const Reply meta = Http(L"GET", L"www.googleapis.com",
                            ToWide("/drive/v3/files/" + UrlEncode(id) + "?supportsAllDrives=true&fields=" +
                                   UrlEncode("id,name,mimeType,size,modifiedTime")),
                            auth);
    if (meta.status != 200) {
        return Fail("Drive could not open the file: " + ApiError(meta));
    }
    const Json info = Json::parse(meta.body, nullptr, false);
    const std::string name = JsonStr(info, "name");
    const std::string mime = JsonStr(info, "mimeType");

    std::string path;
    if (mime.starts_with(kGoogleDocMime)) {
        const std::string exported = ExportType(mime);
        if (mime == "application/vnd.google-apps.folder") {
            return Fail("This is a folder: list it with drive_search, folder = its id.");
        }
        if (exported.empty()) {
            return Fail("This Google file (" + mime + ") has no text form to read.");
        }
        path = "/drive/v3/files/" + UrlEncode(id) + "/export?mimeType=" + UrlEncode(exported);
    } else if (TextFile(mime, name)) {
        path = "/drive/v3/files/" + UrlEncode(id) + "?alt=media&supportsAllDrives=true";
    } else {
        return Fail("\"" + name + "\" is a binary file (" + mime + "); only text files and Google Docs, Sheets and "
                    "Slides can be read.");
    }
    const Reply content = Http(L"GET", L"www.googleapis.com", ToWide(path), auth);
    if (content.status != 200) {
        return Fail("Drive could not give the file: " + ApiError(content));
    }

    const Json& offsetValue = JsonAt(input, "offset");
    const Json& limitValue = JsonAt(input, "limit");
    const size_t total = content.body.size();
    const size_t begin = CharStart(content.body, std::min<size_t>(offsetValue.is_number_integer() ? std::max(0, offsetValue.get<int>()) : 0, total));
    const size_t want = limitValue.is_number_integer() ? static_cast<size_t>(std::clamp(limitValue.get<int>(), 1, static_cast<int>(kReadBytes))) : kReadBytes;
    const size_t end = CharStart(content.body, std::min(total, begin + want));
    std::string text = std::format("# {} ({}, changed {})\n", name, mime, JsonStr(info, "modifiedTime"));
    text.append(content.body, begin, end - begin);
    if (end < total) {
        text += std::format("\n[bytes {}-{} of {}{}; read on with offset={}]", begin, end, total,
                            total >= kFetchCap ? " fetched (the file is longer, only its first 4 MB are reachable)" : "", end);
    }
    return {std::move(text), false};
}

}  // namespace aic
