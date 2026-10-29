#include "core/gdrive.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>
#include <mutex>
#include <utility>
#include <vector>

#include "core/oauth.h"

namespace aic {
namespace {

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

// Google's complaint in a sentence: {"error":{"message":..}} from the API, {"error":"..","error_description":..}
// from the token endpoint.
std::string ApiError(const HttpsReply& reply) {
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

// Where a UTF-8 text may be cut at `pos`: back to the start of the character.
size_t CharStart(const std::string& text, size_t pos) {
    while (pos > 0 && pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

}  // namespace

std::string GoogleDrive::UrlEncode(std::string_view text) {
    return aic::UrlEncode(text);
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
    const std::string authorize = "https://accounts.google.com/o/oauth2/v2/auth?" +
                                  FormBody({{"client_id", clientId},
                                            {"response_type", "code"},
                                            {"scope", kScope},
                                            {"access_type", "offline"},
                                            {"prompt", "consent"}});
    std::string refresh;
    const auto exchange = [&](const OAuthGrant& grant) {
        const HttpsReply reply = HttpsRequest(L"POST", L"oauth2.googleapis.com", L"/token", kFormHeaders,
                                              FormBody({{"client_id", clientId},
                                                        {"client_secret", clientSecret},
                                                        {"code", grant.code},
                                                        {"code_verifier", grant.verifier},
                                                        {"redirect_uri", grant.redirect},
                                                        {"grant_type", "authorization_code"}}));
        const Json json = Json::parse(reply.body, nullptr, false);
        refresh = reply.status == 200 ? JsonStr(json, "refresh_token") : std::string();
        if (refresh.empty()) {
            error = "Google's token request failed: " + ApiError(reply);
            return OAuthPage("Google Диск не подключился: " + ApiError(reply), false);
        }
        return OAuthPage("Google Диск подключён. Эту вкладку можно закрыть.", true);
    };
    if (BrowserSignIn(authorize, "", "Google", exchange, cancel, kSignInWait, error) != BrowserEnd::Answered) {
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
        const HttpsReply reply = HttpsRequest(L"POST", L"oauth2.googleapis.com", L"/token", kFormHeaders,
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
    const HttpsReply reply = HttpsRequest(L"GET", L"www.googleapis.com", ToWide(path), ToWide("Authorization: Bearer " + token + "\r\n"));
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
    const HttpsReply meta = HttpsRequest(L"GET", L"www.googleapis.com",
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
    const HttpsReply content = HttpsRequest(L"GET", L"www.googleapis.com", ToWide(path), auth);
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
