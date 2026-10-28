#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <wil/resource.h>

#include "core/agent_process.h"
#include "core/claude_process.h"
#include "core/loopback_server.h"

namespace aic {

// grok.exe (Grok Build) as an ACP agent over stdio: `grok agent stdio`, JSON-RPC 2.0 one message per line.
// The chat window knows Claude's stream-json; this class translates ACP into it:
//   agent_message_chunk       -> stream_event (text_delta)
//   session/request_permission -> control_request can_use_tool   (answered through Respond)
//   tool_call_update          -> user / tool_result
//   session/prompt's answer   -> result
// Our five tools reach Grok as an MCP server over HTTP on the loopback (LoopbackServer), and each request
// of it goes out as Claude's control_request mcp_message, so ToolHost serves both CLIs alike.
// Grok's own tools are cut down by an agent profile to the two that reach MCP.
// ClaudeOptions is reused: exe (grok.exe), cwd, proxy, model, effort, resume, appendSystemPrompt (the rules).
class GrokProcess : public AgentProcess {
public:
    GrokProcess() = default;
    ~GrokProcess() override;
    GrokProcess(const GrokProcess&) = delete;
    GrokProcess& operator=(const GrokProcess&) = delete;

    // Stops a running process first. Throws std::runtime_error when grok.exe cannot start.
    void Start(const ClaudeOptions& options, MessageHandler onMessage, ExitHandler onExit);
    void Stop() override;

    void SendUserText(std::string_view text) override;
    void SendUserContent(Json content) override;
    void Interrupt() override;
    void SetModel(const std::string& model) override;
    void Respond(const std::string& requestId, Json response) override;
    void RespondError(const std::string& requestId, std::string_view error) override;

private:
    using Callback = std::function<void(const Json& result, const Json& error)>;

    // JSON-RPC out.
    void Call(const char* method, Json params, Callback done);
    void Notify(const char* method, Json params);
    void Reply(const Json& id, Json result);
    void WriteLine(const Json& message);

    // In.
    void ReadLoop();
    void ReadErrors();
    void HandleLine(std::string_view line);
    void OnResponse(const Json& message);
    void OnNotification(const std::string& method, const Json& params);
    void OnRequest(const Json& id, const std::string& method, const Json& params);
    void OnUpdate(const Json& update);

    // The handshake and the turns.
    void OpenSession(const Json& initResult);
    void MarkReady();
    void Pump();
    void FinishTurn(const Json& result, const Json& error);
    void Emit(Json message);
    void EmitText(const std::string& text);
    void EmitToolResult(const Json& update);
    void EmitState(const char* state);
    std::string HandleMcpRequest(const std::string& body);
    void Watchdog();

    wil::unique_handle process_;
    wil::unique_handle job_;
    wil::unique_handle stdoutRead_;
    wil::unique_handle stderrRead_;
    std::thread reader_;
    std::thread errorReader_;  // joined by reader_
    std::thread watchdog_;
    std::atomic<bool> stopping_ = false;

    std::mutex writeMutex_;
    wil::unique_handle stdinWrite_;  // guarded by writeMutex_

    std::mutex stderrMutex_;
    std::string stderrTail_;  // guarded by stderrMutex_

    LoopbackServer server_;
    std::string token_;
    uint16_t serverPort_ = 0;
    std::wstring profilePath_;

    ClaudeOptions options_;
    MessageHandler onMessage_;
    ExitHandler onExit_;

    // Everything below: guarded by stateMutex_.
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    int64_t nextId_ = 0;
    std::map<int64_t, Callback> calls_;
    std::string sessionId_;
    bool sessionOpen_ = false;   // session/new answered
    bool mcpReady_ = false;      // Grok has connected to our tools (or we stopped waiting for it)
    bool turnActive_ = false;
    std::deque<Json> queue_;     // prompts (ACP content blocks) waiting for the session or the turn
    std::string pendingModel_;   // SetModel before the session was open
    bool textOpen_ = false;      // a streamed text block is running
    unsigned messageCounter_ = 0;
    std::string messageId_;
    unsigned permissionCounter_ = 0;
    std::map<std::string, Json> permissions_;  // our request id -> {id (the agent's), options, tool, input}
    struct Refusal {  // a call of ours the user said no to: let through by ACP, refused by our MCP server
        std::string tool;     // our name: run_command
        Json input;
        std::string message;  // what Grok reads as the call's result
    };
    std::vector<Refusal> refusals_;
    unsigned mcpCounter_ = 0;
    struct McpWait {
        bool done = false;
        std::string reply;
    };
    std::map<std::string, McpWait> mcp_;
};

// %USERPROFILE%\.grok\bin\grok.exe (the installer's place), else PATH; empty if none.
std::wstring FindGrokExe();

}  // namespace aic
