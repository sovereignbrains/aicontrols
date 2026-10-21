#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace aic {

// What the CLI says about the account (`claude auth status`).
struct AuthStatus {
    bool known = false;     // false: the CLI could not be asked (not found, no answer)
    bool loggedIn = false;
    std::string email;
    std::string plan;       // "pro", "max" ... as the CLI names it; empty when not signed in
    std::string method;     // "claude.ai" or an API console
    std::string error;      // why it is not known
};

struct CliResult {
    bool started = false;
    bool timedOut = false;
    bool cancelled = false;
    DWORD exitCode = 1;
    std::string output;  // stdout and stderr together
};

// Runs a command line without a window and waits for it. Its standard input is empty, its output is collected
// (and passed to onOutput as it comes). The CLI goes through `proxy` (http://host:port) when it is not empty.
// Only the process itself is killed on timeout or cancel: a browser it opened is not ours to close.
CliResult RunCli(const std::wstring& commandLine, const std::wstring& proxy, int timeoutSeconds,
                 const std::atomic<bool>* cancel = nullptr,
                 const std::function<void(const std::string&)>& onOutput = {},
                 const std::vector<std::pair<std::wstring, std::wstring>>& extraEnvironment = {},
                 const std::wstring& cwd = {}, const std::wstring& inputFile = {});

AuthStatus ParseAuthStatus(const std::string& output);
// `claude auth status`
AuthStatus QueryAuthStatus(const std::wstring& claudeExe, const std::wstring& proxy);
// The first https:// link in the CLI's login output (the page to open when the browser did not), or "".
std::string FindLoginUrl(const std::string& output);

}  // namespace aic
