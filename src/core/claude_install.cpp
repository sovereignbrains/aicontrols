#include "core/claude_install.h"

#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "core/auth.h"
#include "core/claude_process.h"
#include "core/grok_process.h"

namespace aic {
namespace {

constexpr wchar_t kHost[] = L"downloads.claude.ai";
constexpr wchar_t kBasePath[] = L"/claude-code-releases";

using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

// "http://127.0.0.1:2080" -> "127.0.0.1:2080", the form WinHTTP wants.
std::wstring ProxyHostPort(std::wstring proxy) {
    if (const size_t scheme = proxy.find(L"://"); scheme != std::wstring::npos) {
        proxy.erase(0, scheme + 3);
    }
    while (!proxy.empty() && proxy.back() == L'/') {
        proxy.pop_back();
    }
    return proxy;
}

// GET https://<host><path> (downloads.claude.ai unless named); the body goes to `sink` in pieces (false from it stops the download).
// `total` is the Content-Length, 0 when unknown. Returns the HTTP status, or 0 with `error` set.
DWORD Get(const std::wstring& proxy, const std::wstring& path, std::string& error,
          const std::function<bool(const char*, DWORD, unsigned long long total)>& sink, const wchar_t* host = kHost) {
    const std::wstring hostPort = ProxyHostPort(proxy);
    unique_hinternet session(WinHttpOpen(
        L"AIControls", proxy.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
        proxy.empty() ? WINHTTP_NO_PROXY_NAME : hostPort.c_str(), WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "WinHttpOpen failed";
        return 0;
    }
    WinHttpSetTimeouts(session.get(), 15000, 15000, 30000, 60000);
    unique_hinternet connection(WinHttpConnect(session.get(), host, INTERNET_DEFAULT_HTTPS_PORT, 0));
    unique_hinternet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
    if (!request) {
        error = "Cannot open the request";
        return 0;
    }
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "No answer from " + ToUtf8(host) + " (error " + std::to_string(GetLastError()) + ")";
        return 0;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    unsigned long long total = 0;
    std::array<wchar_t, 32> length{};
    DWORD lengthSize = static_cast<DWORD>(length.size() * sizeof(wchar_t));
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length.data(),
                            &lengthSize, WINHTTP_NO_HEADER_INDEX)) {
        total = _wcstoui64(length.data(), nullptr, 10);
    }
    std::vector<char> chunk(1 << 16);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got)) {
            error = "The download broke off (error " + std::to_string(GetLastError()) + ")";
            return 0;
        }
        if (got == 0) {
            break;
        }
        if (!sink(chunk.data(), got, total)) {
            error = "Stopped";
            return 0;
        }
    }
    return status;
}

bool GetText(const std::wstring& proxy, const std::wstring& path, std::string& text, std::string& error,
             const wchar_t* host = kHost) {
    text.clear();
    const DWORD status = Get(
        proxy, path, error,
        [&](const char* data, DWORD size, unsigned long long) {
            if (text.size() < (1u << 20)) {
                text.append(data, size);
            }
            return true;
        },
        host);
    if (status == 0) {
        return false;
    }
    if (status != 200) {
        error = ToUtf8(host) + " answered " + std::to_string(status) + " for " + ToUtf8(path);
        return false;
    }
    return true;
}

bool IsVersion(std::string_view text) {
    if (text.empty() || text.size() > 40) {
        return false;
    }
    size_t dots = 0;
    for (const char c : text) {
        if (c == '.') {
            ++dots;
        } else if (c < '0' || c > '9') {
            return false;
        }
    }
    return dots == 2 && text.front() != '.' && text.back() != '.' && text.find("..") == std::string_view::npos;
}

std::string Platform() {
    USHORT process = 0;
    USHORT native = 0;
    if (IsWow64Process2(GetCurrentProcess(), &process, &native) && native == IMAGE_FILE_MACHINE_ARM64) {
        return "win32-arm64";
    }
    return "win32-x64";
}

std::wstring TempFile(const std::string& version, const std::string& platform) {
    std::array<wchar_t, MAX_PATH + 1> dir{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
    return std::wstring(dir.data(), length) + L"aic-claude-" + ToWide(version) + L"-" + ToWide(platform) + L".exe";
}

// https://<host><path> into `file`, its progress told as it comes. False with `error` set otherwise.
bool Download(const std::wstring& proxy, const wchar_t* host, const std::wstring& path, const std::wstring& file,
              const std::atomic<bool>* cancel, const InstallProgress& progress, std::string& error) {
    wil::unique_hfile out(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                      nullptr));
    if (!out) {
        error = "Cannot write " + ToUtf8(file);
        return false;
    }
    unsigned long long received = 0;
    unsigned lastPercent = 101;
    const DWORD status = Get(
        proxy, path, error,
        [&](const char* data, DWORD size, unsigned long long total) {
            if (cancel != nullptr && cancel->load()) {
                return false;
            }
            DWORD written = 0;
            if (!WriteFile(out.get(), data, size, &written, nullptr) || written != size) {
                return false;
            }
            received += size;
            const auto percent =
                total > 0 ? static_cast<unsigned>(std::min<unsigned long long>(100, received * 100 / total)) : 0u;
            if (progress && percent != lastPercent) {
                lastPercent = percent;
                progress(percent, false);
            }
            return true;
        },
        host);
    if (status == 0) {
        if (error == "Stopped") {
            error = cancel != nullptr && cancel->load() ? "Cancelled" : "Cannot write " + ToUtf8(file);
        }
        return false;
    }
    if (status != 200) {
        error = ToUtf8(host) + " answered " + std::to_string(status) + " for " + ToUtf8(path);
        return false;
    }
    return true;
}

}  // namespace

std::string ParseManifestChecksum(const std::string& manifest, const std::string& platform) {
    const Json parsed = Json::parse(manifest, nullptr, false);
    if (!parsed.is_object()) {
        return {};
    }
    const std::string sum = JsonStr(JsonAt(JsonAt(parsed, "platforms"), platform.c_str()), "checksum");
    if (sum.size() != 64 || sum.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
        return {};
    }
    std::string lower = sum;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lower;
}

ClaudeRelease LatestClaudeRelease(const std::wstring& proxy) {
    ClaudeRelease release;
    std::string text;
    if (!GetText(proxy, std::wstring(kBasePath) + L"/latest", text, release.error)) {
        return release;
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    if (!IsVersion(text)) {  // an error page, not a version
        release.error = "downloads.claude.ai did not answer with a version";
        return release;
    }
    std::string manifest;
    if (!GetText(proxy, std::wstring(kBasePath) + L"/" + ToWide(text) + L"/manifest.json", manifest, release.error)) {
        return release;
    }
    release.sha256 = ParseManifestChecksum(manifest, Platform());
    if (release.sha256.empty()) {
        release.error = "The manifest has no checksum for " + Platform();
        return release;
    }
    release.version = text;
    return release;
}

std::string ParseClaudeVersion(const std::string& output) {
    size_t start = 0;
    while (start < output.size()) {
        while (start < output.size() && !std::isdigit(static_cast<unsigned char>(output[start]))) {
            ++start;
        }
        size_t end = start;
        while (end < output.size() && (std::isdigit(static_cast<unsigned char>(output[end])) || output[end] == '.')) {
            ++end;
        }
        if (IsVersion(std::string_view(output).substr(start, end - start))) {
            return output.substr(start, end - start);
        }
        start = std::max(end, start + 1);
    }
    return {};
}

std::string InstalledClaudeVersion(const std::wstring& exe) {
    if (exe.empty()) {
        return {};
    }
    const CliResult run = RunCli(L"\"" + exe + L"\" --version", {}, 20);
    return run.started && run.exitCode == 0 ? ParseClaudeVersion(run.output) : std::string();
}

bool ClaudeVersionLess(std::string_view older, std::string_view newer) {
    if (!IsVersion(older) || !IsVersion(newer)) {
        return false;
    }
    const auto parts = [](std::string_view text) {
        std::array<unsigned long long, 3> out{};
        size_t index = 0;
        for (const char c : text) {
            if (c == '.') {
                ++index;
            } else {
                out[index] = out[index] * 10 + static_cast<unsigned>(c - '0');
            }
        }
        return out;
    };
    return parts(older) < parts(newer);
}

std::string FileSha256(const std::wstring& path) {
    wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        return {};
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    const auto closeAlgorithm = wil::scope_exit([&] { BCryptCloseAlgorithmProvider(algorithm, 0); });
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        return {};
    }
    const auto destroyHash = wil::scope_exit([&] { BCryptDestroyHash(hash); });
    std::vector<unsigned char> buffer(1 << 20);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
            return {};
        }
        if (got == 0) {
            break;
        }
        if (BCryptHashData(hash, buffer.data(), got, 0) != 0) {
            return {};
        }
    }
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) != 0) {
        return {};
    }
    std::string hex;
    for (const unsigned char byte : digest) {
        char two[3];
        snprintf(two, sizeof(two), "%02x", byte);
        hex += two;
    }
    return hex;
}

bool InstallClaude(const std::wstring& proxy, const std::atomic<bool>* cancel, const InstallProgress& progress,
                   std::string& error) {
    const ClaudeRelease release = LatestClaudeRelease(proxy);
    if (release.version.empty()) {
        error = release.error;
        return false;
    }
    const std::string platform = Platform();
    const std::wstring temp = TempFile(release.version, platform);
    const auto removeTemp = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });
    const std::wstring path = std::wstring(kBasePath) + L"/" + ToWide(release.version) + L"/" + ToWide(platform) +
                              L"/claude.exe";
    if (!Download(proxy, kHost, path, temp, cancel, progress, error)) {
        return false;
    }
    if (FileSha256(temp) != release.sha256) {
        error = "The checksum of the download does not match the manifest; nothing was installed";
        return false;
    }
    if (progress) {
        progress(100, true);
    }
    const CliResult run = RunCli(L"\"" + temp + L"\" install", proxy, 300, cancel);
    if (!run.started || run.exitCode != 0) {
        std::string tail = run.output.size() > 300 ? run.output.substr(run.output.size() - 300) : run.output;
        error = run.cancelled ? "Cancelled" : "The installer failed (exit code " + std::to_string(run.exitCode) + "): " + tail;
        return false;
    }
    if (FindClaudeExe().empty()) {
        error = "The installer finished but claude.exe is not in place";
        return false;
    }
    return true;
}

std::wstring SignerOf(const std::wstring& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    std::wstring name;
    if (WinVerifyTrust(nullptr, &action, &data) == ERROR_SUCCESS) {
        const CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        const CRYPT_PROVIDER_SGNR* signer =
            provider != nullptr ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0)
                                : nullptr;
        if (signer != nullptr && signer->csCertChain > 0 && signer->pasCertChain[0].pCert != nullptr) {
            std::array<wchar_t, 256> buffer{};
            CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, buffer.data(),
                               static_cast<DWORD>(buffer.size()));
            name = buffer.data();
        }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return name;
}

bool InstallGrok(const std::wstring& proxy, const std::atomic<bool>* cancel, const InstallProgress& progress,
                 std::string& error) {
    struct Source {
        const wchar_t* host;
        const wchar_t* base;
    };
    constexpr std::array<Source, 2> kSources = {{{L"x.ai", L"/cli"},
                                                 {L"storage.googleapis.com", L"/grok-build-public-artifacts/cli"}}};
    const Source* source = nullptr;
    std::string version;
    for (const Source& candidate : kSources) {
        std::string text;
        if (!GetText(proxy, std::wstring(candidate.base) + L"/stable", text, error, candidate.host)) {
            continue;
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
            text.pop_back();
        }
        if (IsVersion(text)) {
            source = &candidate;
            version = text;
            break;
        }
        error = ToUtf8(candidate.host) + " did not answer with a version";
    }
    if (source == nullptr) {
        return false;
    }
    std::array<wchar_t, MAX_PATH + 1> dir{};
    const DWORD tempLength = GetTempPathW(static_cast<DWORD>(dir.size()), dir.data());
    const std::wstring temp = std::wstring(dir.data(), tempLength) + L"aic-grok-" + ToWide(version) + L".exe";
    const auto removeTemp = wil::scope_exit([&] { DeleteFileW(temp.c_str()); });
    const std::wstring arch = Platform() == "win32-arm64" ? L"windows-aarch64" : L"windows-x86_64";
    if (!Download(proxy, source->host, std::wstring(source->base) + L"/grok-" + ToWide(version) + L"-" + arch + L".exe",
                  temp, cancel, progress, error)) {
        return false;
    }
    if (SignerOf(temp) != L"X Corp.") {
        error = "The download is not signed by X Corp.; nothing was installed";
        return false;
    }
    if (progress) {
        progress(100, true);
    }
    std::array<wchar_t, 2048> profile{};
    const DWORD profileLength = GetEnvironmentVariableW(L"USERPROFILE", profile.data(), static_cast<DWORD>(profile.size()));
    if (profileLength == 0 || profileLength >= profile.size()) {
        error = "No USERPROFILE";
        return false;
    }
    const std::filesystem::path home = std::filesystem::path(std::wstring(profile.data(), profileLength)) / L".grok";
    std::error_code failed;
    std::filesystem::create_directories(home / L"bin", failed);
    for (const wchar_t* name : {L"grok.exe", L"agent.exe"}) {
        // A running exe cannot be overwritten but can be moved aside, as its installer does.
        const std::filesystem::path target = home / L"bin" / name;
        const std::filesystem::path aside = target.wstring() + L".old";
        std::filesystem::remove(aside, failed);
        if (!CopyFileW(temp.c_str(), target.c_str(), FALSE)) {
            MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
            if (!CopyFileW(temp.c_str(), target.c_str(), FALSE)) {
                error = "Cannot write " + ToUtf8(target.wstring()) + " (error " + std::to_string(GetLastError()) + ")";
                return false;
            }
        }
    }
    // What its installer leaves for `grok update`: the CLI was put in place by hand, not by a package manager.
    if (const std::filesystem::path config = home / L"config.toml"; !std::filesystem::exists(config, failed)) {
        wil::unique_hfile out(CreateFileW(config.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                          nullptr));
        constexpr char kConfig[] = "[cli]\r\ninstaller = \"internal\"\r\n";
        DWORD written = 0;
        if (out) {
            WriteFile(out.get(), kConfig, sizeof(kConfig) - 1, &written, nullptr);
        }
    }
    if (FindGrokExe().empty()) {
        error = "The download is in place but grok.exe is not found";
        return false;
    }
    return true;
}

}  // namespace aic