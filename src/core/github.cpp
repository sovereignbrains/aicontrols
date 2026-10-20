#include "core/github.h"

#include <windows.h>

#include <winhttp.h>

#include <filesystem>
#include <format>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "core/auth.h"
#include "core/claude_process.h"

namespace aic {
namespace {

using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

// One GET to api.github.com; the body on HTTP 200, else empty with `error` set.
std::string GithubGet(const std::wstring& path, const std::string& token, std::string& error) {
    unique_hinternet session(WinHttpOpen(L"AIControls", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "WinHttpOpen failed";
        return {};
    }
    WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000);
    unique_hinternet connection(WinHttpConnect(session.get(), L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    unique_hinternet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
    if (!request) {
        error = "cannot reach api.github.com";
        return {};
    }
    const std::wstring headers = L"Authorization: Bearer " + ToWide(token) +
                                 L"\r\nAccept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = std::format("request failed (error {})", GetLastError());
        return {};
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &got) || got == 0) {
            break;
        }
        body.append(chunk.data(), got);
    }
    if (status != 200) {
        error = std::format("GitHub answered {}", status);
        if (status == 401) {
            error += ": the token is not accepted";
        }
        return {};
    }
    return body;
}

}  // namespace

std::vector<GithubRepo> ParseGithubRepos(const std::string& json) {
    std::vector<GithubRepo> repos;
    const Json data = Json::parse(json, nullptr, false);
    if (!data.is_array()) {
        return repos;
    }
    for (const Json& item : data) {
        GithubRepo repo;
        repo.fullName = JsonStr(item, "full_name");
        repo.name = JsonStr(item, "name");
        repo.cloneUrl = JsonStr(item, "clone_url");
        repo.description = JsonStr(item, "description");
        const Json& isPrivate = JsonAt(item, "private");
        repo.isPrivate = isPrivate.is_boolean() && isPrivate.get<bool>();
        if (!repo.name.empty() && repo.cloneUrl.starts_with("https://")) {
            repos.push_back(std::move(repo));
        }
    }
    return repos;
}

std::vector<GithubRepo> ListGithubRepos(const std::string& token, std::string& error) {
    std::vector<GithubRepo> all;
    if (token.empty()) {
        error = "no GITHUB secret";
        return all;
    }
    for (int page = 1; page <= 3; ++page) {
        const std::string body = GithubGet(
            std::format(L"/user/repos?per_page=100&sort=pushed&affiliation=owner,collaborator,organization_member&page={}", page),
            token, error);
        if (body.empty()) {
            return page == 1 ? std::vector<GithubRepo>{} : all;
        }
        std::vector<GithubRepo> repos = ParseGithubRepos(body);
        const size_t count = repos.size();
        all.insert(all.end(), std::make_move_iterator(repos.begin()), std::make_move_iterator(repos.end()));
        if (count < 100) {
            break;
        }
    }
    error.clear();
    return all;
}

std::wstring RepoFolder(const GithubRepo& repo) {
    wchar_t profile[MAX_PATH]{};
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    return (std::filesystem::path(profile) / L"projects" / ToWide(repo.name)).wstring();
}

bool CloneGithubRepo(const GithubRepo& repo, const std::wstring& folder, const std::string& token,
                     const std::atomic<bool>* cancel, std::string& output) {
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(folder).parent_path(), ignored);
    // git reads extra config from GIT_CONFIG_COUNT/KEY_n/VALUE_n: here an Authorization header for github.com.
    std::string basic;
    {
        const std::string pair = "x-access-token:" + token;
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (size_t i = 0; i < pair.size(); i += 3) {
            const auto byte = [&](size_t k) { return k < pair.size() ? static_cast<unsigned char>(pair[k]) : 0u; };
            const unsigned chunk = byte(i) << 16 | byte(i + 1) << 8 | byte(i + 2);
            basic += alphabet[(chunk >> 18) & 63];
            basic += alphabet[(chunk >> 12) & 63];
            basic += i + 1 < pair.size() ? alphabet[(chunk >> 6) & 63] : '=';
            basic += i + 2 < pair.size() ? alphabet[chunk & 63] : '=';
        }
    }
    const std::vector<std::pair<std::wstring, std::wstring>> environment = {
        {L"GIT_CONFIG_COUNT", L"1"},
        {L"GIT_CONFIG_KEY_0", L"http.https://github.com/.extraheader"},
        {L"GIT_CONFIG_VALUE_0", L"AUTHORIZATION: basic " + ToWide(basic)},
        {L"GIT_TERMINAL_PROMPT", L"0"},  // never wait for a password
        {L"GCM_INTERACTIVE", L"never"},
    };
    const CliResult run = RunCli(L"git clone \"" + ToWide(repo.cloneUrl) + L"\" \"" + folder + L"\"", L"", 900, cancel, {},
                                 environment);
    output = run.output;
    return run.started && !run.cancelled && !run.timedOut && run.exitCode == 0 &&
           std::filesystem::exists(std::filesystem::path(folder) / L".git", ignored);
}

}  // namespace aic
