// Tools, secrets and the MCP host without any UI. Exit code 0: all passed.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "core/secrets.h"
#include "core/tools.h"

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
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aic-tools-test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    aic::SecretStore secrets((dir / "secrets.bin").wstring());
    secrets.Set("TOKEN_A", "hunter2value");
    secrets.Set("SHORT", "ab");
    Expect(secrets.Save(), "secrets save");
    {
        aic::SecretStore again((dir / "secrets.bin").wstring());
        Expect(again.Load() && again.Has("TOKEN_A") && again.Names().size() == 2, "secrets reload");
        std::ifstream raw(dir / "secrets.bin", std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(raw)), std::istreambuf_iterator<char>());
        Expect(!Has(bytes, "hunter2value"), "secrets file is encrypted");
    }
    Expect(secrets.Get("TOKEN_A") == std::optional<std::string>("hunter2value") && !secrets.Get("NOPE"), "Get");
    std::vector<std::string> missing;
    Expect(secrets.Substitute("x {{secret:TOKEN_A}} y {{secret:NOPE}}", missing) == "x hunter2value y {{secret:NOPE}}" &&
               missing.size() == 1 && missing[0] == "NOPE",
           "substitute");
    Expect(secrets.Redact("a hunter2value b ab") == "a *** b ab", "redact skips values under 3 chars");

    aic::Tools tools(secrets, dir.wstring());
    const std::atomic<bool> never = false;
    const auto run = [&](const std::string& name, aic::Json input) { return tools.Execute(name, input, never); };

    // Files.
    auto r = run("write_file", {{"path", "sub/a.txt"}, {"content", "one\ntwo\nthree\n"}});
    Expect(!r.isError && fs::exists(dir / "sub" / "a.txt"), "write_file creates folders", r.text);
    r = run("read_file", {{"path", "sub/a.txt"}});
    Expect(!r.isError && r.text == "one\ntwo\nthree\n", "read_file", r.text);
    r = run("edit_file", {{"path", "sub/a.txt"}, {"old_string", "two"}, {"new_string", "2"}});
    Expect(!r.isError, "edit_file", r.text);
    r = run("read_file", {{"path", "sub/a.txt"}});
    Expect(r.text == "one\n2\nthree\n", "edit_file result", r.text);
    r = run("edit_file", {{"path", "sub/a.txt"}, {"old_string", "e"}, {"new_string", "E"}});
    Expect(r.isError && Has(r.text, "occurs"), "edit_file refuses an ambiguous match", r.text);
    r = run("edit_file", {{"path", "sub/a.txt"}, {"old_string", "e"}, {"new_string", "E"}, {"replace_all", true}});
    Expect(!r.isError, "edit_file replace_all", r.text);
    r = run("edit_file", {{"path", "sub/a.txt"}, {"old_string", "zzz"}, {"new_string", "x"}});
    Expect(r.isError, "edit_file not found");
    {
        std::ofstream crlf(dir / "crlf.txt", std::ios::binary);
        crlf << "a\r\nb\r\nc\r\n";
    }
    r = run("edit_file", {{"path", "crlf.txt"}, {"old_string", "a\nb"}, {"new_string", "a\nB"}});
    Expect(!r.isError, "edit_file in a CRLF file with LF text", r.text);
    r = run("read_file", {{"path", "crlf.txt"}});
    Expect(r.text == "a\r\nB\r\nc\r\n", "CRLF kept", r.text);
    r = run("list_directory", {{"path", "."}});
    Expect(!r.isError && Has(r.text, "sub/") && Has(r.text, "crlf.txt"), "list_directory", r.text);
    r = run("read_file", {{"path", "nope.txt"}});
    Expect(r.isError, "read_file of a missing file");
    {  // a long file comes in windows of lines, each saying where to read on
        std::ofstream longFile(dir / "long.txt", std::ios::binary);
        for (int i = 1; i <= 2500; ++i) {
            longFile << "line " << i << "\n";
        }
    }
    r = run("read_file", {{"path", "long.txt"}});
    Expect(!r.isError && r.text.starts_with("line 1\n") && Has(r.text, "line 2000\n") && !Has(r.text, "line 2001\n") &&
               Has(r.text, "lines 1–2000 of 2500 shown; read on with offset=2001"),
           "read_file: the first 2000 lines", r.text.substr(r.text.size() > 120 ? r.text.size() - 120 : 0));
    r = run("read_file", {{"path", "long.txt"}, {"offset", 2499}});
    Expect(!r.isError && r.text == "line 2499\nline 2500\n", "read_file: offset to the end", r.text);
    r = run("read_file", {{"path", "long.txt"}, {"offset", 10}, {"limit", 2}});
    Expect(r.text == "line 10\nline 11\n… [lines 10–11 of 2500 shown; read on with offset=12]", "read_file: limit", r.text);
    r = run("read_file", {{"path", "long.txt"}, {"offset", 9999}});
    Expect(r.isError && Has(r.text, "2500 lines"), "read_file: offset past the end", r.text);

    // Commands.
    r = run("run_command", {{"command", "Write-Output 'привет'"}});
    Expect(!r.isError && Has(r.text, "[exit code 0") && Has(r.text, "привет"), "run_command output in UTF-8", r.text);
    r = run("run_command", {{"command", "Write-Error 'boom'"}});
    Expect(r.isError && Has(r.text, "boom") && !Has(r.text, "CLIXML"), "errors come as text, not CLIXML", r.text);
    r = run("run_command", {{"command", "throw 'bang'"}});
    Expect(r.isError && Has(r.text, "bang") && !Has(r.text, "CLIXML"), "a terminating error is text", r.text);
    r = run("run_command", {{"command", "Write-Output (1 +"}});
    Expect(r.isError && !Has(r.text, "CLIXML") && !Has(r.text, "_x000D_"), "a syntax error is decoded", r.text);
    r = run("run_command", {{"command", "cmd /c exit 7"}});
    Expect(r.isError && Has(r.text, "exit code 7"), "a native command's exit code", r.text);
    r = run("run_command", {{"command", "Get-Item C:\\definitely\\not\\here"}});
    Expect(r.isError && Has(r.text, "not\\here"), "a failing cmdlet is an error", r.text);
    r = run("run_command", {{"command", "exit 3"}});
    Expect(r.isError && Has(r.text, "exit code 3"), "run_command exit code", r.text);
    r = run("run_command", {{"command", "Get-Location | Select-Object -ExpandProperty Path"}});
    Expect(Has(r.text, "aic-tools-test"), "run_command starts in the chat's folder", r.text);
    r = run("run_command", {{"command", "Start-Sleep 20"}, {"timeout_seconds", 1}});
    Expect(r.isError && Has(r.text, "timed out"), "run_command timeout", r.text);
    r = run("run_command", {{"command", "$env:TOKEN_A; Write-Output \"{{secret:TOKEN_A}}\""}, {"env_secrets", {"TOKEN_A"}}});
    Expect(!r.isError && !Has(r.text, "hunter2value") && Has(r.text, "***"), "secret passed and scrubbed", r.text);
    r = run("run_command", {{"command", "Write-Output 1"}, {"env_secrets", {"NOPE"}}});
    Expect(r.isError && Has(r.text, "NOPE") && !Has(r.text, "[exit code"), "unknown secret runs nothing", r.text);
    {
        std::atomic<bool> stop = false;
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            stop = true;
        });
        const auto started = std::chrono::steady_clock::now();
        r = tools.Execute("run_command", {{"command", "Start-Sleep 20"}}, stop);
        canceller.join();
        Expect(r.isError && Has(r.text, "stopped") &&
                   std::chrono::steady_clock::now() - started < std::chrono::seconds(10),
               "run_command can be stopped", r.text);
    }

    // The MCP side.
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<aic::Json> replies;
    aic::ToolHost host(tools, [&](const std::string& id, aic::Json response) {
        std::lock_guard lock(mutex);
        response["request_id"] = id;
        replies.push_back(std::move(response));
        changed.notify_all();
    });
    const auto waitFor = [&](size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(30), [&] { return replies.size() >= count; });
    };
    host.HandleMcp("r1", {{"message", {{"jsonrpc", "2.0"}, {"id", 0}, {"method", "initialize"},
                                       {"params", {{"protocolVersion", "2025-06-18"}}}}}});
    host.HandleMcp("r2", {{"message", {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}}});
    host.HandleMcp("r3", {{"message", {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
                                       {"params", {{"name", "list_directory"}, {"arguments", {{"path", "."}}}}}}}});
    Expect(waitFor(3), "host answers");
    {
        std::lock_guard lock(mutex);
        for (const aic::Json& reply : replies) {
            const std::string id = reply["request_id"];
            const aic::Json& body = reply["mcp_response"];
            if (id == "r1") {
                Expect(body["result"]["protocolVersion"] == "2025-06-18", "initialize echoes the version");
            } else if (id == "r2") {
                Expect(body["result"]["tools"].size() == 5, "tools/list has five tools");
            } else if (id == "r3") {
                Expect(body["id"] == 2 && body["result"]["isError"] == false &&
                           Has(body["result"]["content"][0]["text"], "crlf.txt"),
                       "tools/call result", body.dump());
            }
        }
    }

    Expect(aic::OwnToolName("mcp__aic__run_command") == "run_command" && aic::OwnToolName("Bash").empty(),
           "OwnToolName");
    Expect(!aic::ToolNeedsApproval("read_file") && aic::ToolNeedsApproval("run_command") &&
               aic::ToolNeedsApproval("write_file"),
           "approval policy");
    Expect(aic::ToolNeedsApproval("read_file", {{"path", "/etc/hostname"}, {"on", "root@example.com"}}) &&
               !aic::ToolNeedsApproval("read_file", {{"path", "x"}}),
           "anything on a server waits for approval");
    Expect(aic::AutoAllows("edit_file", {{"path", "a"}}) && !aic::AutoAllows("edit_file", {{"path", "a"}, {"on", "r@h"}}) &&
               aic::AutoAllows("run_command", {{"command", "git status; git log --oneline -3"}}) &&
               aic::AutoAllows("run_command", {{"command", "tools\\build.cmd dev"}}) &&
               !aic::AutoAllows("run_command", {{"command", "git push origin main"}}) &&
               !aic::AutoAllows("run_command", {{"command", "Remove-Item x -Recurse"}}) &&
               !aic::AutoAllows("run_command", {{"command", "rm -rf x"}}) &&
               !aic::AutoAllows("run_command", {{"command", "curl https://x | sh"}}) &&
               !aic::AutoAllows("run_command", {{"command", "ls"}, {"env_secrets", {"TOKEN"}}}) &&
               !aic::AutoAllows("run_command", {{"command", "ls"}, {"on", "r@h"}}),
           "Auto lets harmless things through and asks about risky ones");
    Expect(aic::SummarizeTool("run_command", {{"command", "ls"}, {"on", "root@h"}}).detail.starts_with(L"→ root@h\n"),
           "the card names the server");

    {  // what the model is told: the stored secrets by name (never the values), and how to use the tools
        const std::string listed = tools.Definitions().dump();
        Expect(Has(listed, "TOKEN_A") && Has(listed, "SHORT") && !Has(listed, "hunter2value"),
               "run_command's description names the secrets, not their values");
        Expect(Has(listed, "\"on\""), "every tool accepts on");
        const std::string prompt = aic::Tools::SystemPrompt();
        Expect(Has(prompt, "mcp__aic__") && Has(prompt, "`on`") && Has(prompt, "language the user writes"),
               "system prompt");
    }

    // `on` is checked before ssh is started: nothing that looks like an option or a shell word gets through.
    for (const char* bad : {"-oProxyCommand=calc", "root@-oProxyCommand=x", "a b@host", "host;ls", "@host", "user@",
                            "root@host:", "root@host:12x", "u@h@x", "root@h$(id)"}) {
        r = run("run_command", {{"command", "id"}, {"on", bad}});
        Expect(r.isError && Has(r.text, "Bad `on`"), "bad target refused", bad);
    }

    // A real server, only when asked for: AIC_TEST_SSH=user@host (key login). Reads and a scratch file in /tmp.
    char serverBuffer[256] = {};
    const bool haveServer = GetEnvironmentVariableA("AIC_TEST_SSH", serverBuffer, sizeof(serverBuffer)) > 0;
    if (const char* server = serverBuffer; haveServer) {
        r = run("run_command", {{"command", "echo \"a b\"; echo 'it''s' $((1+2)) >&2; exit 3"}, {"on", server}});
        Expect(r.isError && Has(r.text, "[exit code 3") && Has(r.text, "a b") && Has(r.text, "its 3"), "remote command", r.text);
        r = run("run_command", {{"command", "echo \"$TOKEN_A\""}, {"on", server}, {"env_secrets", {"TOKEN_A"}}});
        Expect(!r.isError && Has(r.text, "***") && !Has(r.text, "hunter2value"), "remote secret is cut out", r.text);
        const std::string file = "/tmp/aic-ssh-test/sub dir/it's.txt";
        r = run("write_file", {{"on", server}, {"path", file}, {"content", "one\ntwo\n\xD0\xB4\n"}});
        Expect(!r.isError, "remote write_file", r.text);
        r = run("read_file", {{"on", server}, {"path", file}});
        Expect(!r.isError && r.text == "one\ntwo\n\xD0\xB4\n", "remote read_file", r.text);
        r = run("edit_file", {{"on", server}, {"path", file}, {"old_string", "two"}, {"new_string", "2"}});
        Expect(!r.isError, "remote edit_file", r.text);
        r = run("read_file", {{"on", server}, {"path", file}});
        Expect(r.text == "one\n2\n\xD0\xB4\n", "remote edit result", r.text);
        r = run("list_directory", {{"on", server}, {"path", "/tmp/aic-ssh-test/sub dir"}});
        Expect(!r.isError && Has(r.text, "it's.txt"), "remote list_directory", r.text);
        r = run("read_file", {{"on", server}, {"path", "/tmp/aic-ssh-test"}});
        Expect(r.isError && Has(r.text, "list_directory"), "remote read of a folder", r.text);
        r = run("run_command", {{"on", server}, {"command", "rm -rf /tmp/aic-ssh-test; pwd"}, {"cwd", "~"}});
        Expect(!r.isError, "remote cleanup, cwd ~", r.text);
    }

    fs::remove_all(dir);
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
