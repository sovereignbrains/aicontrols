#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace aic {

// A chat as claude.exe keeps it: ~\.claude\projects\<workspace>\<id>.jsonl; or as grok.exe does:
// ~\.grok\sessions\<workspace, %-encoded>\<id>\chat_history.jsonl.
struct SessionInfo {
    std::string id;
    std::wstring title;  // the CLI's own title (custom, else the generated one); may be empty
    FILETIME modified{};
    bool grok = false;  // Grok's: only grok.exe can go on with it
};

enum class EntryKind { User, Assistant, Tool, Compacted };  // Compacted: the CLI summed up what came before

struct TranscriptImage {
    std::string mediaType;
    std::string base64;
};

struct TranscriptEntry {
    EntryKind kind = EntryKind::User;
    std::wstring text;  // the tool's name for EntryKind::Tool
    std::vector<TranscriptImage> images;  // EntryKind::User: the pictures sent with it
    // EntryKind::Tool: the call as it was made (the input as JSON text) and what came back.
    std::string toolId;
    std::string toolInput;
    std::wstring toolOutput;
    bool toolFailed = false;
    bool toolAnswered = false;
};

// How the sidebar lists projects. Recent: the one open now first, then by the newest chat.
// Name: A to Z. Added: the order they were put on the list.
enum class ProjectOrder { Recent, Name, Added };

struct ProjectPlace {
    std::wstring folder;
    std::wstring name;
    uint64_t activity = 0;  // newest chat, FILETIME ticks; a live unsaved chat sits above any file
};

// `general` stays last (its own section, not a project among the others). Recent keeps `open` first, then the
// newest activity, and breaks ties by the order given. Name is the user's language, ignoring case.
std::vector<ProjectPlace> OrderProjects(std::vector<ProjectPlace> projects, const std::wstring& general,
                                        const std::wstring& open, ProjectOrder order);
ProjectOrder ParseProjectOrder(const std::string& text);  // unknown, and empty: Recent
const char* ProjectOrderId(ProjectOrder order);

struct Settings {
    std::string model;   // a value from the CLI's model list; empty: its default
    std::string effort;  // low … max; empty: the model's default
    // Claude's model list as last reported (JSON), so a start on a Grok model still offers the way back.
    std::string claudeModels;
    // Grok's fast tier (its "…-build-fast" model, twice the price): what the Grok row picks.
    bool grokFast = false;
    // What runs without a card: "" asks before anything that changes something, "edits" writes files on this
    // computer unasked (commands and servers still ask), "all" asks nothing.
    std::string permissions;
    // The same for Grok's chats, apart (28.10.2026, his: Claude may ask nothing, Grok is not known to be safe yet).
    std::string grokPermissions;
    // The claude.ai connectors (Notion, Drive, Calendar…) and other MCP servers in every chat: their tools'
    // descriptions ride in every request (~140k tokens against ~7k without them, measured 22.10.2026), so off.
    bool connectors = false;
    // The plan's limits as last reported (the CLI reports them only once a turn has run), so the ring
    // is not empty at start. Percent -1: never seen; resets are Unix seconds.
    int sessionPercent = -1;
    long long sessionResets = 0;
    int weekPercent = -1;
    long long weekResets = 0;
    // The side panels' widths as the user dragged them, in DIPs; 0: the default.
    float sidebarWidth = 0;
    float terminalWidth = 0;
    // The folder the chats work in (empty: DefaultWorkspace).
    std::wstring workspace;
    // The sidebar's projects: the folders chats work in, in the order they were added (the general one,
    // DefaultWorkspace, is always there and not among them), and the ones folded there.
    std::vector<std::wstring> projects;
    std::vector<std::wstring> closedProjects;
    // Which of those orders the sidebar shows. The list above stays the added order, so "added" can come back.
    ProjectOrder projectOrder = ProjectOrder::Recent;
    // The Claude model before the last move to Grok: a Claude chat opened from the history goes on in it.
    // Empty: the CLI's default.
    std::string claudeModel;
    // A chat to open again at the next start (set by an update, which restarts the client); cleared once opened.
    std::string reopen;
};

// The old SSH Client's workspace while both clients live side by side: Claude's memory and the
// chat history are keyed by this folder. Falls back to the profile folder.
std::wstring DefaultWorkspace();

// Claude's memory for a workspace: ~\.claude\projects\<workspace>\memory.
std::wstring MemoryFolder(const std::wstring& workspace);
// The one memory every project's chats share (the CLI is pointed at it with autoMemoryDirectory): the general
// workspace's, which is kept in its own repository.
std::wstring SharedMemoryFolder();

// Where grok.exe keeps a workspace's chats, a folder each: the one there, else the one it would make.
std::wstring GrokHistoryFolder(const std::wstring& workspace);

// Claude's chats and Grok's, newest first. A Grok session nothing was said in is left out: each start opens one.
std::vector<SessionInfo> ListSessions(const std::wstring& workspace);
std::wstring SessionTitle(const std::wstring& workspace, const std::string& id);
// Moves a chat to the Recycle Bin: its transcript and the folder the CLI keeps beside it (Grok's: its folder).
// False if it could not.
bool DeleteSession(const std::wstring& workspace, const std::string& id);
// Moves a chat to another project: its transcript and its folder go to that workspace's history, where the CLI
// resumes it (checked 22.10.2026: the whole conversation comes back). False if it could not; nothing is lost then.
// Grok's chats are not moved (false): grok.exe keeps them by folder, and its resume elsewhere is not checked.
bool MoveSession(const std::wstring& from, const std::wstring& to, const std::string& id);
// Gives a chat its own title the way the CLI's /rename does: a custom-title entry appended to its file
// (the newest one wins, over the generated title too). Grok's: a file of ours in its folder, its own left as they are.
bool RenameSession(const std::wstring& workspace, const std::string& id, const std::wstring& title);
// The last `limit` entries of a chat, for showing it again before it is resumed.
std::vector<TranscriptEntry> LoadTranscript(const std::wstring& workspace, const std::string& id, size_t limit,
                                            bool& truncated);

Settings LoadSettings();
void SaveSettings(const Settings& settings);
// A window that only shows samples (--gallery) must not write the settings of the real one.
void SetSettingsReadOnly(bool readOnly);

}  // namespace aic
