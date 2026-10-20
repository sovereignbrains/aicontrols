#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

namespace aic {

struct ClaudeRelease {
    std::string version;  // "2.1.302"
    std::string sha256;   // of this machine's Windows build, lower case hex
    std::string error;    // why there is no version
};

// The newest release on downloads.claude.ai (the place the official installer reads), with the checksum of the
// Windows build that fits this machine. Goes through `proxy` (http://host:port) when it is not empty.
ClaudeRelease LatestClaudeRelease(const std::wstring& proxy);

// The checksum of `platform` ("win32-x64") in a release's manifest.json; empty when it is not there.
std::string ParseManifestChecksum(const std::string& manifest, const std::string& platform);

// "2.1.302 (Claude Code)" -> "2.1.302"; empty when the text holds no version.
std::string ParseClaudeVersion(const std::string& output);
// `claude --version`; empty when the CLI could not be asked.
std::string InstalledClaudeVersion(const std::wstring& exe);
// 2.1.99 < 2.1.302. Anything that is not dotted numbers counts as unknown (never "less").
bool ClaudeVersionLess(std::string_view older, std::string_view newer);

// SHA-256 of a file as lower case hex; empty when it cannot be read.
std::string FileSha256(const std::wstring& path);

// What the installer is doing, for a progress line: 0..100 while it downloads, then 100 with `installing`.
using InstallProgress = std::function<void(unsigned percent, bool installing)>;

// The official installer's steps: the newest build is downloaded, its SHA-256 checked against the manifest,
// and `claude.exe install` puts it in %USERPROFILE%\.local\bin. True when claude.exe is there afterwards;
// otherwise `error` says why (also when `cancel` was raised).
bool InstallClaude(const std::wstring& proxy, const std::atomic<bool>* cancel, const InstallProgress& progress,
                   std::string& error);

}  // namespace aic
