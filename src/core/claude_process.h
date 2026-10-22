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

namespace aic {

using Json = nlohmann::json;

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
};

// claude.exe in stream-json mode (see PLAN.md, "Протокол"): one process per chat, serving
// turn after turn while its stdin stays open. Both handlers run on the reader thread.
class ClaudeProcess {
public:
    using MessageHandler = std::function<void(Json)>;
    using ExitHandler = std::function<void(DWORD exitCode, std::string stderrTail)>;

    ClaudeProcess() = default;
    ~ClaudeProcess();
    ClaudeProcess(const ClaudeProcess&) = delete;
    ClaudeProcess& operator=(const ClaudeProcess&) = delete;

    // Stops a running process first. Throws std::runtime_error when claude.exe cannot start.
    void Start(const ClaudeOptions& options, MessageHandler onMessage, ExitHandler onExit);
    // Closes stdin, gives the CLI a moment to finish, then kills its whole process tree.
    // No handler runs after this returns.
    void Stop();

    void SendUserText(std::string_view text);
    // A message as content blocks: pictures, documents, text.
    void SendUserContent(Json content);
    void Interrupt();
    void Respond(const std::string& requestId, Json response);
    void RespondError(const std::string& requestId, std::string_view error);

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

// %USERPROFILE%\.local\bin\claude.exe (the native installer's place), else PATH; empty if none.
std::wstring FindClaudeExe();
// The way out for the CLI's HTTPS: the old client's bridge (2092), else sing-box's mixed port
// (2080), else direct (empty).
std::wstring PickProxy();

std::string ToUtf8(std::wstring_view text);
std::wstring ToWide(std::string_view text);

// Lenient JSON access: a missing key or a wrong type gives "" / an empty object.
std::string JsonStr(const Json& object, const char* key);
const Json& JsonAt(const Json& object, const char* key);

}  // namespace aic
