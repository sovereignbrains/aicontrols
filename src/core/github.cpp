#include "core/github.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <format>

#include <nlohmann/json.hpp>

#include "core/auth.h"
#include "core/claude_process.h"
#include "core/oauth.h"
#include "github_app.h"

namespace aic {
namespace {

// The rights a token from the sign-in carries: the repositories, private ones too, and their workflows (a push
// that touches .github/workflows needs it).
constexpr std::string_view kGithubScope = "repo workflow";
constexpr auto kGithubWait = std::chrono::minutes(3);

// One GET to api.github.com; the body on HTTP 200, else empty with `error` set and `status` (0: no answer).
std::string GithubGet(const std::wstring& path, const std::string& token, std::string& error, int* status = nullptr) {
    const std::wstring headers = L"Authorization: Bearer " + ToWide(token) +
                                 L"\r\nAccept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    const HttpsReply reply = HttpsRequest(L"GET", L"api.github.com", path, headers);
    if (status) {
        *status = reply.status;
    }
    if (reply.status == 0) {
        error = "cannot reach api.github.com: " + reply.error;
        return {};
    }
    if (reply.status != 200) {
        error = std::format("GitHub answered {}", reply.status);
        if (reply.status == 401) {
            error += ": the token is not accepted";
        }
        return {};
    }
    return reply.body;
}

}  // namespace

GithubApp BuiltGithubApp() {
    return {kBuiltGithubClientId, kBuiltGithubClientSecret};
}

GithubLogin WhoIsGithub(const std::string& token) {
    GithubLogin who;
    if (token.empty()) {
        who.rejected = true;
        who.error = "no GITHUB secret";
        return who;
    }
    int status = 0;
    const std::string body = GithubGet(L"/user", token, who.error, &status);
    who.rejected = status == 401;
    who.login = JsonStr(Json::parse(body, nullptr, false), "login");
    if (!body.empty() && who.login.empty()) {
        who.error = "GitHub's answer has no login";
    }
    return who;
}

GithubSignIn SignInGithub(const GithubApp& app, const std::atomic<bool>& cancel) {
    GithubSignIn result;
    if (app.clientId.empty() || app.clientSecret.empty()) {
        result.error = "this build has no GitHub app: set GITHUB_OAUTH_ID and GITHUB_OAUTH_SECRET in the secrets";
        return result;
    }
    const std::string authorize =
        "https://github.com/login/oauth/authorize?" + FormBody({{"client_id", app.clientId}, {"scope", std::string(kGithubScope)}});
    const auto exchange = [&](const OAuthGrant& grant) {
        const HttpsReply reply = HttpsRequest(L"POST", L"github.com", L"/login/oauth/access_token",
                                              std::wstring(kFormHeaders) + L"Accept: application/json\r\n",
                                              FormBody({{"client_id", app.clientId},
                                                        {"client_secret", app.clientSecret},
                                                        {"code", grant.code},
                                                        {"redirect_uri", grant.redirect},
                                                        {"code_verifier", grant.verifier}}));
        const Json data = Json::parse(reply.body, nullptr, false);
        result.token = JsonStr(data, "access_token");
        if (result.token.empty()) {  // GitHub says no with a 200 and {error, error_description}
            const std::string why = JsonStr(data, "error_description");
            result.error = "GitHub's token request failed: " +
                           (!why.empty() ? why : reply.status == 0 ? reply.error : std::format("HTTP {}", reply.status));
            return OAuthPage("GitHub не подключился: " + result.error, false);
        }
        const GithubLogin who = WhoIsGithub(result.token);
        result.login = who.login;
        return OAuthPage(who.login.empty() ? std::string("GitHub подключён. Эту вкладку можно закрыть.")
                                           : "GitHub подключён: " + who.login + ". Эту вкладку можно закрыть.",
                         true);
    };
    std::string error;
    if (BrowserSignIn(authorize, "/callback", "GitHub", exchange, cancel, kGithubWait, error) != BrowserEnd::Answered) {
        result.error = error;
        result.cancelled = cancel;
    }
    return result;
}

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

std::vector<std::pair<std::wstring, std::wstring>> GithubGitEnvironment(const std::string& token) {
    // git reads extra config from GIT_CONFIG_COUNT/KEY_n/VALUE_n: here an Authorization header for github.com.
    std::string basic;
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
    return {
        {L"GIT_CONFIG_COUNT", L"1"},
        {L"GIT_CONFIG_KEY_0", L"http.https://github.com/.extraheader"},
        {L"GIT_CONFIG_VALUE_0", L"AUTHORIZATION: basic " + ToWide(basic)},
        {L"GIT_TERMINAL_PROMPT", L"0"},  // never wait for a password
        {L"GCM_INTERACTIVE", L"never"},
    };
}

MemorySave SaveMemory(const std::wstring& folder, const std::string& token) {
    MemorySave result;
    std::error_code error;
    if (!std::filesystem::exists(std::filesystem::path(folder) / L".git", error)) {
        result.output = "the memory folder is not a git repository: " + ToUtf8(folder);
        return result;
    }
    const auto environment = GithubGitEnvironment(token);
    const std::wstring git = L"git -C \"" + folder + L"\" ";
    const auto run = [&](const std::wstring& args, int timeout) {
        CliResult step = RunCli(git + args, L"", timeout, nullptr, {}, environment);
        result.output += step.output;
        return step;
    };
    run(L"add -A", 60);
    // Named here, not in the clone's config: a fresh clone has none.
    const CliResult commit = run(std::format(L"-c user.name=claude-memory -c user.email=claude-memory@localhost commit -q "
                                             L"-m \"Memory: {:%Y-%m-%d %H:%M}\"",
                                             std::chrono::zoned_time(std::chrono::current_zone(),
                                                                     std::chrono::floor<std::chrono::minutes>(
                                                                         std::chrono::system_clock::now()))),
                                 60);
    const bool committed = commit.started && commit.exitCode == 0;
    CliResult push = run(L"push -q -u origin HEAD", 120);
    if (push.exitCode != 0 && push.output.find("rejected") != std::string::npos) {  // changed elsewhere first
        if (run(L"pull -q --rebase origin HEAD", 120).exitCode == 0) {
            push = run(L"push -q -u origin HEAD", 120);
        }
    }
    result.ok = push.started && !push.timedOut && push.exitCode == 0;
    result.nothing = result.ok && !committed;
    return result;
}
bool CloneGithubRepo(const GithubRepo& repo, const std::wstring& folder, const std::string& token,
                     const std::atomic<bool>* cancel, std::string& output) {
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(folder).parent_path(), ignored);
    const auto environment = GithubGitEnvironment(token);
    const CliResult run = RunCli(L"git clone \"" + ToWide(repo.cloneUrl) + L"\" \"" + folder + L"\"", L"", 900, cancel, {},
                                 environment);
    output = run.output;
    return run.started && !run.cancelled && !run.timedOut && run.exitCode == 0 &&
           std::filesystem::exists(std::filesystem::path(folder) / L".git", ignored);
}

}  // namespace aic
