#include "core/auth.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "core/claude_process.h"

namespace aic {

CliResult RunCli(const std::wstring& commandLine, const std::wstring& proxy, int timeoutSeconds,
                 const std::atomic<bool>* cancel, const std::function<void(const std::string&)>& onOutput,
                 const std::vector<std::pair<std::wstring, std::wstring>>& extraEnvironment, const std::wstring& cwd,
                 const std::wstring& inputFile) {
    CliResult result;
    std::vector<std::pair<std::wstring, std::wstring>> overrides = extraEnvironment;
    if (!proxy.empty()) {
        overrides.insert(overrides.end(), {{L"HTTPS_PROXY", proxy}, {L"HTTP_PROXY", proxy},
                                           {L"NO_PROXY", L"127.0.0.1,localhost,::1"}});
    }
    std::vector<wchar_t> environment = MakeEnvironmentBlock(overrides);

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    wil::unique_handle outRead, outWrite;
    if (!CreatePipe(outRead.put(), outWrite.put(), &inheritable, 0)) {
        return result;
    }
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    // Standard input: the file when one is given, else nothing.
    wil::unique_handle nul(CreateFileW(inputFile.empty() ? L"NUL" : inputFile.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nul) {
        return result;
    }

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<std::byte> attributesBuffer(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        return result;
    }
    auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
    std::array<HANDLE, 2> inherited{nul.get(), outWrite.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(inherited), nullptr, nullptr)) {
        return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul.get();
    startup.StartupInfo.hStdOutput = outWrite.get();
    startup.StartupInfo.hStdError = outWrite.get();
    startup.lpAttributeList = attributes;

    std::wstring command = commandLine;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info)) {
        return result;
    }
    const wil::unique_handle process(info.hProcess);
    const wil::unique_handle thread(info.hThread);
    outWrite.reset();
    result.started = true;

    // Peeking instead of a blocking read: a program the CLI started may keep the pipe open after the CLI is
    // gone, and the end is the process's, not the pipe's.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    std::vector<char> chunk(8192);
    const auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(outRead.get(), nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (!::ReadFile(outRead.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) || got == 0) {
                return;
            }
            result.output.append(chunk.data(), got);
            if (onOutput) {
                onOutput(std::string(chunk.data(), got));
            }
        }
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(process.get(), 50) == WAIT_OBJECT_0) {
            drain();
            break;
        }
        if (cancel != nullptr && cancel->load()) {
            result.cancelled = true;
            TerminateProcess(process.get(), 1);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = true;
            TerminateProcess(process.get(), 1);
            break;
        }
    }
    GetExitCodeProcess(process.get(), &result.exitCode);
    return result;
}

AuthStatus ParseAuthStatus(const std::string& output) {
    AuthStatus status;
    const size_t brace = output.find('{');
    const Json data = brace == std::string::npos ? Json() : Json::parse(output.substr(brace), nullptr, false);
    if (!data.is_object()) {
        status.error = output.substr(0, 300);
        return status;
    }
    status.known = true;
    const Json& loggedIn = JsonAt(data, "loggedIn");
    status.loggedIn = loggedIn.is_boolean() && loggedIn.get<bool>();
    status.email = JsonStr(data, "email");
    status.plan = JsonStr(data, "subscriptionType");
    status.method = JsonStr(data, "authMethod");
    return status;
}

AuthStatus QueryAuthStatus(const std::wstring& claudeExe, const std::wstring& proxy) {
    if (claudeExe.empty()) {
        AuthStatus status;
        status.error = "claude.exe not found";
        return status;
    }
    const CliResult run = RunCli(L"\"" + claudeExe + L"\" auth status", proxy, 30);
    if (!run.started) {
        AuthStatus status;
        status.error = "cannot start claude.exe";
        return status;
    }
    return ParseAuthStatus(run.output);
}

std::string FindLoginUrl(const std::string& output) {
    const size_t start = output.find("https://");
    if (start == std::string::npos) {
        return {};
    }
    size_t end = start;
    while (end < output.size() && static_cast<unsigned char>(output[end]) > 0x20) {
        ++end;
    }
    return output.substr(start, end - start);
}

}  // namespace aic
