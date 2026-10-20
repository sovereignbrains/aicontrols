// Real PowerShell in a pseudo console, shown through the emulator. Exit code 0: all passed.
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "core/pty.h"
#include "core/vt_screen.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << (detail.empty() ? "" : "\n  " + detail) << "\n";
    }
}

std::string Narrow(const std::wstring& text) {
    std::string out;
    for (const wchar_t c : text) {
        out += c < 128 ? static_cast<char>(c) : '?';
    }
    return out;
}

}  // namespace

int main() {
    std::mutex mutex;
    std::condition_variable changed;
    aic::VtScreen screen(100, 30);
    aic::Pty pty;
    screen.SetReply([&](std::string text) { pty.Write(text); });
    bool exited = false;
    DWORD exitCode = 99;

    const auto dump = [&] {
        std::string all;
        for (size_t i = 0; i < screen.LineCount(); ++i) {
            all += Narrow(screen.TextBetween(i, 0, i, screen.Cols())) + "\n";
        }
        return all;
    };
    const auto waitFor = [&](const std::string& needle, int seconds) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(seconds),
                                [&] { return dump().find(needle) != std::string::npos; });
    };

    std::wstring command = L"powershell.exe -NoLogo -NoProfile";
    if (wchar_t custom[512]; GetEnvironmentVariableW(L"AIC_PTY_CMD", custom, 512) > 0) {
        command = custom;
    }
    pty.Start(
        100, 30, command, L"", [&](std::string bytes) {
            if (GetEnvironmentVariableA("AIC_PTY_DEBUG", nullptr, 0) > 0) {
                std::string shown;
                for (const char c : bytes.substr(0, 300)) {
                    shown += static_cast<unsigned char>(c) < 32 ? "\\x" + std::to_string(static_cast<int>(c)) + " " : std::string(1, c);
                }
                std::cerr << "[data " << bytes.size() << "] " << shown << "\n";
            }
            std::lock_guard lock(mutex);
            screen.Feed(bytes);
            changed.notify_all();
        },
        [&](DWORD code) {
            std::lock_guard lock(mutex);
            exited = true;
            exitCode = code;
            changed.notify_all();
        });

    Expect(waitFor("PS ", 15), "the prompt appears");
    pty.Write("Write-Output ('hello-' + 'from-pty')\r");
    Expect(waitFor("hello-from-pty", 10), "a command's output reaches the screen", dump());

    pty.Write("Write-Host -ForegroundColor Red 'redtext'\r");
    Expect(waitFor("redtext", 10), "colored output");
    {
        std::lock_guard lock(mutex);
        bool red = false;
        for (size_t i = 0; i < screen.LineCount() && !red; ++i) {
            const aic::VtLine& line = screen.Line(i);
            for (size_t col = 0; col + 6 < line.size(); ++col) {
                if (line[col].ch == U'r' && line[col + 1].ch == U'e' && line[col + 2].ch == U'd' &&
                    line[col + 3].ch == U't' && line[col].fg != aic::kVtDefault && line[col].fg != 0xcccccc) {
                    red = true;
                    break;
                }
            }
        }
        Expect(red, "the text is colored");
    }

    pty.Resize(60, 20);
    {
        std::lock_guard lock(mutex);
        screen.Resize(60, 20);
    }
    pty.Write("Write-Output 'after-resize'\r");
    Expect(waitFor("after-resize", 10), "still works after a resize");

    pty.Write("exit 7\r");
    {
        std::unique_lock lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(10), [&] { return exited; }), "the exit is reported");
        Expect(exitCode == 7, "exit code", std::to_string(exitCode));
    }
    pty.Stop();
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
