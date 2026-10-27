// Manual end-to-end probe (spends one short request): the real grok.exe with our MCP tools.
// Usage: aic_grok_probe.exe "<prompt>" [model]    Allows every tool call and prints what happens.
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <string>

#include "core/grok_process.h"
#include "core/secrets.h"
#include "core/store.h"
#include "core/tools.h"

int wmain(int argc, wchar_t** argv) {
    // The prompt is argv[1], or the AIC_PROMPT variable (PowerShell 5.1 mangles quotes in native arguments).
    std::wstring prompt = argc >= 2 ? argv[1] : L"";
    std::wstring fromEnvironment(32768, L'\0');
    fromEnvironment.resize(GetEnvironmentVariableW(L"AIC_PROMPT", fromEnvironment.data(),
                                                   static_cast<DWORD>(fromEnvironment.size())));
    if (!fromEnvironment.empty()) {
        prompt = fromEnvironment;
    }
    if (prompt.empty()) {
        std::cerr << "usage: aic_grok_probe <prompt> [model]   (or set AIC_PROMPT)\n";
        return 2;
    }
    aic::SecretStore secrets(aic::SecretStore::DefaultPath());
    secrets.Load();
    const std::wstring workspace = aic::DefaultWorkspace();
    aic::Tools tools(secrets, workspace);
    aic::GrokProcess grok;
    aic::ToolHost host(tools, [&](const std::string& id, aic::Json response) { grok.Respond(id, std::move(response)); });

    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    int exitCode = 1;
    bool sent = false;

    aic::ClaudeOptions options;
    options.cwd = workspace;
    options.appendSystemPrompt = aic::Tools::SystemPrompt();  // as the window starts it
    options.proxy = aic::PickProxy();
    options.model = argc >= 3 ? aic::ToUtf8(argv[2]) : "";
    std::cout << "proxy: " << (options.proxy.empty() ? "direct" : aic::ToUtf8(options.proxy)) << "\n";

    grok.Start(
        options,
        [&](aic::Json message) {
            const std::string type = aic::JsonStr(message, "type");
            if (type == "control_request") {
                const std::string id = aic::JsonStr(message, "request_id");
                const aic::Json& request = aic::JsonAt(message, "request");
                const std::string subtype = aic::JsonStr(request, "subtype");
                if (subtype == "mcp_message") {
                    std::cout << "[mcp] " << aic::JsonStr(aic::JsonAt(request, "message"), "method") << "\n";
                    host.HandleMcp(id, request);
                } else if (subtype == "can_use_tool") {
                    std::cout << "[allow] " << aic::JsonStr(request, "tool_name") << " "
                              << aic::JsonAt(request, "input").dump() << "\n";
                    if (aic::OwnToolName(aic::JsonStr(request, "tool_name")).empty()) {
                        grok.Respond(id, aic::Json{{"behavior", "deny"}, {"message", "not ours"}});
                    } else {
                        grok.Respond(id, aic::Json{{"behavior", "allow"}, {"updatedInput", aic::JsonAt(request, "input")}});
                    }
                } else {
                    grok.RespondError(id, "unsupported");
                }
            } else if (type == "control_response") {
                const aic::Json& answer = aic::JsonAt(aic::JsonAt(message, "response"), "response");
                std::cout << "[models] " << aic::JsonAt(answer, "models").dump().substr(0, 600) << "\n";
            } else if (type == "system") {
                std::cout << "[system] " << message.dump() << "\n";
                if (aic::JsonStr(message, "subtype") == "init" && !sent) {
                    sent = true;
                    grok.SendUserText(aic::ToUtf8(prompt));
                }
            } else if (type == "stream_event") {
                const aic::Json& event = aic::JsonAt(message, "event");
                if (aic::JsonStr(event, "type") == "message_start") {
                    std::cout << "\n[grok] ";
                } else if (aic::JsonStr(event, "type") == "content_block_delta") {
                    std::cout << aic::JsonStr(aic::JsonAt(event, "delta"), "text");
                }
            } else if (type == "user") {
                for (const aic::Json& block : aic::JsonAt(aic::JsonAt(message, "message"), "content")) {
                    if (aic::JsonStr(block, "type") == "tool_result") {
                        std::cout << "\n[tool_result] " << aic::JsonAt(block, "content").dump().substr(0, 400) << "\n";
                    }
                }
            } else if (type == "result") {
                exitCode = aic::JsonStr(message, "subtype") == "success" ? 0 : 1;
                std::cout << "\n[result] " << message.dump() << "\n";
                std::lock_guard lock(mutex);
                finished = true;
                done.notify_all();
            } else {
                std::cout << "[" << type << "] " << message.dump().substr(0, 400) << "\n";
            }
        },
        [&](DWORD code, std::string tail) {
            std::cout << "[exit " << code << "] " << tail << "\n";
            std::lock_guard lock(mutex);
            finished = true;
            done.notify_all();
        });

    std::unique_lock lock(mutex);
    if (!done.wait_for(lock, std::chrono::seconds(120), [&] { return finished; })) {
        std::cout << "timeout\n";
    }
    lock.unlock();
    grok.Stop();
    return exitCode;
}
