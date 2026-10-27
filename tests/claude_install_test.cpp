// The installer's pieces, without any UI. Exit code 0: all passed.
// AIC_TEST_NET=1 also asks downloads.claude.ai for the newest release and installs Grok's CLI into a temporary profile.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/claude_install.h"
#include "core/claude_process.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << (detail.empty() ? "" : "\n  " + detail) << "\n";
    }
}

}  // namespace

int main() {
    using namespace aic;
    Expect(ParseClaudeVersion("2.1.302 (Claude Code)\n") == "2.1.302", "version of --version");
    Expect(ParseClaudeVersion("claude v10.20.3-beta") == "10.20.3", "version inside text");
    Expect(ParseClaudeVersion("no version here 1.2").empty() && ParseClaudeVersion("").empty(), "no version");

    Expect(ClaudeVersionLess("2.1.99", "2.1.302") && ClaudeVersionLess("2.1.302", "2.2.0") &&
               ClaudeVersionLess("1.9.9", "2.0.0"),
           "older versions");
    Expect(!ClaudeVersionLess("2.1.302", "2.1.302") && !ClaudeVersionLess("2.1.302", "2.1.99") &&
               !ClaudeVersionLess("junk", "2.1.0") && !ClaudeVersionLess("2.1.0", ""),
           "equal, newer and unknown are not older");

    const std::string sum(64, 'A');
    const std::string manifest = R"({"version":"2.1.302","platforms":{"win32-x64":{"checksum":")" + sum +
                                 R"(","size":1},"linux-x64":{"checksum":"short"}}})";
    Expect(ParseManifestChecksum(manifest, "win32-x64") == std::string(64, 'a'), "manifest checksum, lower case");
    Expect(ParseManifestChecksum(manifest, "linux-x64").empty() && ParseManifestChecksum(manifest, "win32-arm64").empty() &&
               ParseManifestChecksum("<html>", "win32-x64").empty(),
           "manifest without a usable checksum");

    namespace fs = std::filesystem;
    const fs::path file = fs::temp_directory_path() / "aic-sha-test.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << "abc";
    }
    Expect(FileSha256(file.wstring()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    Expect(FileSha256((fs::temp_directory_path() / "aic-no-such-file").wstring()).empty(), "sha256 of a missing file");
    Expect(SignerOf(file.wstring()).empty(), "an unsigned file has no signer");
    fs::remove(file);
    char profile[MAX_PATH] = {};
    GetEnvironmentVariableA("USERPROFILE", profile, sizeof(profile));
    if (const fs::path grok = fs::path(profile) / ".grok" / "bin" / "grok.exe"; fs::exists(grok)) {
        Expect(SignerOf(grok.wstring()) == L"X Corp.", "the installed grok.exe is signed by X Corp.");
    }

    char net[8] = {};
    if (GetEnvironmentVariableA("AIC_TEST_NET", net, sizeof(net)) > 0) {
        for (const std::wstring& proxy : {std::wstring(), PickProxy()}) {
            const ClaudeRelease release = LatestClaudeRelease(proxy);
            std::cout << (proxy.empty() ? "direct: " : "proxy: ") << (release.version.empty() ? release.error : release.version)
                      << "\n";
            if (release.version.empty() && !proxy.empty()) {
                Expect(false, "latest release through the proxy", release.error);
            } else if (!release.version.empty()) {
                Expect(release.sha256.size() == 64, "latest release has a checksum");
                const std::string installed = InstalledClaudeVersion(FindClaudeExe());
                std::cout << "installed: " << installed << "\n";
                Expect(!installed.empty(), "installed version");
            }
        }
        // Grok's CLI into a profile of its own, so the real ~/.grok is left alone.
        const fs::path home = fs::temp_directory_path() / "aic-grok-home";
        fs::remove_all(home);
        fs::create_directories(home);
        SetEnvironmentVariableW(L"USERPROFILE", home.c_str());
        std::string error;
        const bool ok = InstallGrok(PickProxy(), nullptr, {}, error);
        SetEnvironmentVariableA("USERPROFILE", profile);
        std::cout << "grok: " << (ok ? "installed" : error) << "\n";
        Expect(ok, "Grok's CLI installs", error);
        Expect(fs::exists(home / ".grok" / "bin" / "grok.exe") && fs::exists(home / ".grok" / "bin" / "agent.exe") &&
                   fs::exists(home / ".grok" / "config.toml"),
               "grok.exe, agent.exe and config.toml in place");
        fs::remove_all(home);
    }

    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
