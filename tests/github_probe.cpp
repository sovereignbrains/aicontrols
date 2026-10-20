// By hand: the repositories the GITHUB secret reaches; with "--clone <owner/name> <folder>", a clone of one.
#include <iostream>
#include <string>

#include "core/claude_process.h"
#include "core/github.h"
#include "core/secrets.h"

int wmain(int argc, wchar_t** argv) {
    aic::SecretStore secrets(aic::SecretStore::DefaultPath());
    secrets.Load();
    const std::string token = secrets.Get("GITHUB").value_or("");
    std::string error;
    const auto repos = aic::ListGithubRepos(token, error);
    std::cout << "repos: " << repos.size() << (error.empty() ? "" : ", error: " + error) << "\n";
    for (size_t i = 0; i < repos.size() && i < 15; ++i) {
        std::cout << "  " << repos[i].fullName << (repos[i].isPrivate ? " (private)" : "") << "\n";
    }
    if (argc >= 4 && std::wstring(argv[1]) == L"--clone") {
        for (const auto& repo : repos) {
            if (repo.fullName == aic::ToUtf8(argv[2])) {
                std::string output;
                const bool ok = aic::CloneGithubRepo(repo, argv[3], token, nullptr, output);
                std::cout << (ok ? "cloned\n" : "clone failed\n") << output.substr(0, 600) << "\n";
                return ok ? 0 : 1;
            }
        }
        std::cout << "no such repo\n";
        return 1;
    }
    return error.empty() ? 0 : 1;
}
