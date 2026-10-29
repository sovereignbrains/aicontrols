// By hand: the GitHub sign-in in the browser with the built-in app. Prints the login the token belongs to
// and the first lines of the repositories it reaches; the token itself is neither printed nor stored.
#include <atomic>
#include <iostream>

#include "core/github.h"

int main() {
    const aic::GithubApp app = aic::BuiltGithubApp();
    if (app.clientId.empty()) {
        std::cout << "this build has no GitHub app\n";
        return 2;
    }
    std::atomic<bool> cancel{false};
    std::cout << "browser opens, approve there\n" << std::flush;
    const aic::GithubSignIn in = aic::SignInGithub(app, cancel);
    if (in.token.empty()) {
        std::cout << "failed: " << in.error << (in.cancelled ? " (cancelled)" : "") << "\n";
        return 1;
    }
    std::cout << "login: " << in.login << ", token length: " << in.token.size() << "\n";
    std::string error;
    const auto repos = aic::ListGithubRepos(in.token, error);
    std::cout << "repos: " << repos.size() << (error.empty() ? "" : "  error: " + error) << "\n";
    return 0;
}
