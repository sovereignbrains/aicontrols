#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/claude_process.h"
#include "core/secrets.h"

namespace aic {

inline constexpr const char* kToolServer = "aic";  // the MCP server name: tools reach the model as mcp__aic__<name>

struct ToolResult {
    std::string text;
    bool isError = false;
    std::string imageBase64;  // a picture sent along as an MCP image block (screenshot); empty if none
    std::string imageType;    // image/png or image/jpeg
};

// What the approval card shows for a call.
struct ToolSummary {
    std::wstring title;   // "Выполнить команду", "Записать файл" …
    std::wstring detail;  // the command, the path and the text — what the user is agreeing to
};

// A tool's name as the model sees it (mcp__aic__run_command) → ours (run_command); empty if it is not ours.
std::string OwnToolName(const std::string& fullName);
// Grok's own tools that its agent profile lets through: the web and Imagine (web_search, web_fetch, image_gen,
// image_edit, image_to_video, reference_to_video). GrokProcess hands them on as grok__<name>.
inline constexpr const char* kGrokToolPrefix = "grok__";
bool GrokOwnTool(const std::string& name);
// grok__image_gen → image_gen; empty for anything else.
std::string GrokToolName(const std::string& fullName);
// The tools that change nothing: reading and listing files, and this client's chats.
bool ReadOnlyTool(const std::string& name);
// Reading and listing run without asking; commands and writes wait for the user.
// Anything aimed at a server (`on`) waits too.
bool ToolNeedsApproval(const std::string& name, const Json& input = Json::object());
// The "Auto" permission mode: file edits here, Grok's pictures and commands that look harmless go without asking;
// deleting, installing, the network, secrets, the system, videos and anything aimed at a server still wait.
bool AutoAllows(const std::string& name, const Json& input);
ToolSummary SummarizeTool(const std::string& name, const Json& input);
// The name of the secret that holds a server's password, "ssh user@host[:port]", for what the user typed as a
// secret's name ("ssh root@1.2.3.4", or "root@1.2.3.4" alone); empty when it names no server. `on` with that
// server logs in with the password when no key works.
std::string ServerSecretName(std::string_view text);

// The tools: run_command, read_file, write_file, edit_file, list_directory (the five work on a server too), and
// list_chats and read_chat for the client's own chats.
class Tools {
public:
    Tools(SecretStore& secrets, std::wstring cwd);

    Json Definitions() const;  // the MCP tools/list payload; run_command names the stored secrets
    // What the model is told about this client, appended to the CLI's system prompt.
    static std::string SystemPrompt();
    // Blocks until done; `cancel` stops a running command. Output is scrubbed of secret values.
    ToolResult Execute(const std::string& name, const Json& input, const std::atomic<bool>& cancel) const;

    void SetCwd(std::wstring cwd) { cwd_ = std::move(cwd); }

private:
    ToolResult RunCommand(const Json& input, const std::atomic<bool>& cancel) const;
    // Any of the five on a server, through the system ssh.exe (by key or by its stored password, no prompts).
    ToolResult ExecuteOn(const std::string& on, const std::string& name, const Json& input,
                         const std::atomic<bool>& cancel) const;
    ToolResult ReadFile(const Json& input) const;
    ToolResult WriteFile(const Json& input) const;
    ToolResult EditFile(const Json& input) const;
    ToolResult ListDirectory(const Json& input) const;
    ToolResult Screenshot(const Json& input) const;
    ToolResult ListChats(const Json& input) const;
    ToolResult ReadChat(const Json& input) const;
    std::vector<std::wstring> Projects(const std::string& project) const;  // the given one, else all known
    std::wstring Resolve(const std::string& path) const;

    SecretStore& secrets_;
    std::wstring cwd_;
};

// Answers the CLI's mcp_message requests (initialize, tools/list, tools/call) for our server. A call runs
// on its own thread; `reply` is called from there with the request id and the control_response payload.
class ToolHost {
public:
    using Reply = std::function<void(const std::string& requestId, Json response)>;

    ToolHost(Tools& tools, Reply reply);
    ~ToolHost();
    ToolHost(const ToolHost&) = delete;
    ToolHost& operator=(const ToolHost&) = delete;

    void HandleMcp(const std::string& requestId, const Json& request);
    // Stops running commands; their replies still go out (as errors), the CLI is waiting for them.
    void CancelAll();

private:
    void Reap();

    struct Job {
        std::thread thread;
        std::atomic<bool> done = false;
        std::atomic<bool> cancel = false;
    };

    Tools& tools_;
    Reply reply_;
    std::mutex mutex_;
    std::list<Job> jobs_;  // guarded by mutex_
};

}  // namespace aic
