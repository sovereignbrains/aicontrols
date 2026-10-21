#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

namespace aic {

struct GithubRepo {
    std::string fullName;  // owner/name
    std::string name;
    std::string cloneUrl;  // https://github.com/owner/name.git
    std::string description;
    bool isPrivate = false;
};

// The repositories the token can reach, the most recently pushed first (GitHub's REST API, up to 300).
// On failure the list is empty and `error` says why.
std::vector<GithubRepo> ListGithubRepos(const std::string& token, std::string& error);

// The repositories as the API answers them (one page), for the tests.
std::vector<GithubRepo> ParseGithubRepos(const std::string& json);

// Where a repository is cloned: %USERPROFILE%\projects\<name>.
std::wstring RepoFolder(const GithubRepo& repo);

// What git needs in its environment to reach github.com with the token: an Authorization header, never a prompt.
std::vector<std::pair<std::wstring, std::wstring>> GithubGitEnvironment(const std::string& token);

// Claude's memory folder is a clone of a private repository: everything in it committed and pushed (pulled
// and pushed again when it changed elsewhere first). `nothing`: there was nothing new to save.
struct MemorySave {
    bool ok = false;
    bool nothing = false;
    std::string output;
    // Filled by the caller when Haiku wrote first (WriteMemoryFromChat).
    bool written = true;
    int created = 0;
    int appended = 0;
    std::string writeError;
};
MemorySave SaveMemory(const std::wstring& folder, const std::string& token);

// `git clone` of the repository into `folder`, the token passed to git as an HTTP header through the
// environment (never on a command line, never saved in the clone's config). True when the clone is there.
bool CloneGithubRepo(const GithubRepo& repo, const std::wstring& folder, const std::string& token,
                     const std::atomic<bool>* cancel, std::string& output);

}  // namespace aic
