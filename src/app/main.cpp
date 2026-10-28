#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app/chat_window.h"
#include "core/claude_process.h"
#include "core/secrets.h"

namespace {

// The GUI exe has no console of its own; when started from a script its stdout is a pipe or a file.
void Print(const std::string& text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// aicontrols --import-secrets A,B,C : stores the values of the environment variables A, B, C. This is how
// secrets move in from another program (it passes them as env_secrets); no value is ever printed.
// aicontrols --list-secrets : the names.
int RunSecretsCommand(const std::wstring& command, const std::wstring& argument) {
    aic::SecretStore store(aic::SecretStore::DefaultPath());
    store.Load();
    std::string report;
    if (command == L"--import-secrets") {
        std::stringstream names(aic::ToUtf8(argument));
        std::string name;
        std::vector<wchar_t> value(32768);
        int failed = 0;
        while (std::getline(names, name, ',')) {
            if (name.empty()) {
                continue;
            }
            const DWORD length = GetEnvironmentVariableW(aic::ToWide(name).c_str(), value.data(),
                                                         static_cast<DWORD>(value.size()));
            if (length == 0 || length >= value.size()) {
                report += "missing: " + name + "\n";
                ++failed;
                continue;
            }
            store.Set(name, aic::ToUtf8(std::wstring_view(value.data(), length)));
            report += "stored: " + name + "\n";
        }
        SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        if (!store.Save()) {
            Print("cannot save the secrets file\n");
            return 2;
        }
        Print(report);
        return failed == 0 ? 0 : 1;
    }
    for (const std::string& name : store.Names()) {
        report += name + "\n";
    }
    Print(report.empty() ? "no secrets\n" : report);
    return 0;
}

// ssh's SSH_ASKPASS when a tool logs in to a server by password (RunSsh in core/tools.cpp): AIC_ASKPASS names
// the secret, "ssh user@host", and its value goes to ssh's pipe. Only a password prompt is answered, and only
// with a server's secret, so this is no way to read the others.
int AskPass(const std::wstring& name, std::wstring prompt) {
    std::ranges::transform(prompt, prompt.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (!name.starts_with(L"ssh ") || prompt.find(L"password") == std::wstring::npos) {
        return 1;
    }
    aic::SecretStore store(aic::SecretStore::DefaultPath());
    store.Load();
    std::optional<std::string> value = store.Get(aic::ToUtf8(name));
    if (!value) {
        return 1;
    }
    value->push_back('\n');
    Print(*value);
    SecureZeroMemory(value->data(), value->size());
    return 0;
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show) {
    int count = 0;
    const std::unique_ptr<LPWSTR, decltype(&LocalFree)> args(CommandLineToArgvW(GetCommandLineW(), &count),
                                                              &LocalFree);
    std::wstring askpass(512, L'\0');
    if (const DWORD length = GetEnvironmentVariableW(L"AIC_ASKPASS", askpass.data(), static_cast<DWORD>(askpass.size()));
        length > 0 && length < askpass.size()) {
        askpass.resize(length);
        return AskPass(askpass, args && count >= 2 ? args.get()[1] : L"");
    }
    if (args && count >= 2) {
        const std::wstring command = args.get()[1];
        if (command == L"--import-secrets" && count >= 3) {
            return RunSecretsCommand(command, args.get()[2]);
        }
        if (command == L"--list-secrets") {
            return RunSecretsCommand(command, {});
        }
    }
    return aic::RunChatWindow(instance, show);
}
