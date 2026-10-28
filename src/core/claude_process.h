#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <wil/resource.h>

#include "core/agent_process.h"

namespace aic {

struct ClaudeOptions {
    std::wstring exe;     // claude.exe; empty: FindClaudeExe()
    std::wstring cwd;     // the session's working directory; empty: ours
    std::wstring proxy;   // http://host:port for the CLI's HTTPS; empty: direct
    std::string model;    // empty: the CLI's default
    std::string effort;   // low … max; empty: the model's default
    std::string resume;   // a session id to continue; empty: a new chat
    std::string appendSystemPrompt;  // added to the CLI's own system prompt; empty: nothing
    std::string settings;  // JSON for --settings, over the user's own settings; empty: none
    bool includePartial = true;  // stream_event deltas while a message is written
    // The claude.ai connectors and the user's other MCP servers; off, only ours: their tools' descriptions
    // would ride in every request.
    bool connectors = false;
    std::string autocompact;  // --autocompact: the context size a chat is summed up at ("150k"); empty: the CLI's
    // Off: nothing written to the history (the probes: their runs would sit among his chats).
    bool persistSession = true;
};

// claude.exe in stream-json mode (see PLAN.md, "Протокол"): one process per chat, serving
// turn after turn while its stdin stays open. Both handlers run on the reader thread.
class ClaudeProcess : public AgentProcess {
public:
    ClaudeProcess() = default;
    ~ClaudeProcess() override;
    ClaudeProcess(const ClaudeProcess&) = delete;
    ClaudeProcess& operator=(const ClaudeProcess&) = delete;

    // Stops a running process first. Throws std::runtime_error when claude.exe cannot start.
    void Start(const ClaudeOptions& options, MessageHandler onMessage, ExitHandler onExit);
    // Closes stdin, gives the CLI a moment to finish, then kills its whole process tree.
    // No handler runs after this returns.
    void Stop() override;

    void SendUserText(std::string_view text) override;
    void SendUserContent(Json content) override;
    void Interrupt() override;
    // set_model: no restart.
    void SetModel(const std::string& model) override;
    void Respond(const std::string& requestId, Json response) override;
    void RespondError(const std::string& requestId, std::string_view error) override;

private:
    // afterInit: hold the message until the CLI has answered initialize.
    void Send(const Json& message, bool afterInit);
    void WriteLocked(const std::string& line);
    void ReadLoop();
    void ReadErrors();
    void HandleLine(std::string_view line);

    wil::unique_handle process_;
    wil::unique_handle job_;
    wil::unique_handle stdoutRead_;
    wil::unique_handle stderrRead_;
    std::thread reader_;
    std::thread errorReader_;  // joined by reader_
    std::atomic<bool> stopping_ = false;

    std::mutex writeMutex_;
    wil::unique_handle stdinWrite_;      // guarded by writeMutex_
    bool initialized_ = false;           // guarded by writeMutex_
    std::vector<std::string> pending_;   // guarded by writeMutex_

    std::mutex stderrMutex_;
    std::string stderrTail_;  // guarded by stderrMutex_

    MessageHandler onMessage_;
    ExitHandler onExit_;
    unsigned requestCounter_ = 0;
};

// Our environment plus the overrides, as a CREATE_UNICODE_ENVIRONMENT block for CreateProcessW.
std::vector<wchar_t> MakeEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& overrides);

// One argument for CommandLineToArgvW / the CRT's parser.
std::wstring QuoteArg(std::wstring_view arg);

// A CLI child with its three pipes, in a job that kills it and everything it spawns when closed.
struct SpawnedProcess {
    wil::unique_handle process;
    wil::unique_handle job;
    wil::unique_handle stdinWrite;
    wil::unique_handle stdoutRead;
    wil::unique_handle stderrRead;
};
// `commandLine` is the whole line, exe first. Throws std::runtime_error when it cannot start.
SpawnedProcess SpawnPiped(const std::wstring& exe, std::wstring commandLine,
                          const std::vector<std::pair<std::wstring, std::wstring>>& envOverrides,
                          const std::wstring& cwd);

// %USERPROFILE%\.local\bin\claude.exe (the native installer's place), else PATH; empty if none.
std::wstring FindClaudeExe();
// This very program: ssh asks it for a server's password (AskPass in main.cpp).
std::wstring OwnExe();
// The way out for the CLI's HTTPS: our bridge (core/bridge.h); if it cannot listen, the old client's
// bridge (2092), else sing-box's mixed port (2080), else direct (empty).
std::wstring PickProxy();

std::string ToUtf8(std::wstring_view text);
std::wstring ToWide(std::string_view text);

// Lenient JSON access: a missing key or a wrong type gives "" / an empty object.
std::string JsonStr(const Json& object, const char* key);
const Json& JsonAt(const Json& object, const char* key);

}  // namespace aic
