// The CLI runner and the status parser, without any UI. Exit code 0: all passed.
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

#include "core/auth.h"
#include "core/claude_process.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << (detail.empty() ? "" : "\n  " + detail) << "\n";
    }
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

int main() {
    {
        const aic::AuthStatus s = aic::ParseAuthStatus(
            "{\n  \"loggedIn\": true,\n  \"authMethod\": \"claude.ai\",\n  \"email\": \"a@b.c\",\n  \"subscriptionType\": \"pro\"\n}\n");
        Expect(s.known && s.loggedIn && s.email == "a@b.c" && s.plan == "pro" && s.method == "claude.ai", "status parsed");
    }
    {
        const aic::AuthStatus s = aic::ParseAuthStatus("{\"loggedIn\": false}");
        Expect(s.known && !s.loggedIn && s.email.empty(), "signed out");
    }
    {
        const aic::AuthStatus s = aic::ParseAuthStatus("some warning\n{\"loggedIn\": true, \"email\": \"x@y.z\"}");
        Expect(s.known && s.loggedIn && s.email == "x@y.z", "text before the JSON is skipped");
        Expect(!aic::ParseAuthStatus("not json at all").known, "garbage is not known");
        Expect(!aic::ParseAuthStatus("").known, "empty is not known");
    }
    Expect(aic::FindLoginUrl("Opening browser...\nVisit: https://claude.ai/oauth?x=1&y=2 to continue\n") ==
               "https://claude.ai/oauth?x=1&y=2",
           "login URL");
    Expect(aic::FindLoginUrl("no link here").empty(), "no URL");

    // The runner.
    aic::CliResult r = aic::RunCli(L"cmd.exe /c echo hello-cli& echo to-stderr 1>&2& exit 3", L"", 20);
    Expect(r.started && r.exitCode == 3 && Has(r.output, "hello-cli") && Has(r.output, "to-stderr"),
           "output of both streams and the exit code", r.output);
    std::string streamed;
    r = aic::RunCli(L"cmd.exe /c echo one", L"", 20, nullptr, [&](const std::string& text) { streamed += text; });
    Expect(Has(streamed, "one"), "output is passed on as it comes", streamed);
    r = aic::RunCli(L"cmd.exe /c ping -n 20 127.0.0.1 >nul", L"", 1);
    Expect(r.timedOut, "timeout");
    {
        std::atomic<bool> stop = false;
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            stop = true;
        });
        const auto started = std::chrono::steady_clock::now();
        r = aic::RunCli(L"cmd.exe /c ping -n 20 127.0.0.1 >nul", L"", 60, &stop);
        stopper.join();
        Expect(r.cancelled && std::chrono::steady_clock::now() - started < std::chrono::seconds(10), "cancel");
    }
    r = aic::RunCli(L"definitely-not-a-program.exe", L"", 5);
    Expect(!r.started, "a missing program does not start");
    {  // reading stdin must not hang: it is empty
        r = aic::RunCli(L"cmd.exe /c set /p x=\"\" & echo done-reading", L"", 15);
        Expect(Has(r.output, "done-reading"), "stdin is empty, not the parent's", r.output);
    }

    // The real CLI, when it is installed.
    const std::wstring exe = aic::FindClaudeExe();
    if (!exe.empty()) {
        const aic::AuthStatus s = aic::QueryAuthStatus(exe, L"");
        Expect(s.known, "claude auth status answers", s.error);
    }
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
