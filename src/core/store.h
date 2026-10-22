#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace aic {

// A chat as claude.exe keeps it: ~\.claude\projects\<workspace>\<id>.jsonl.
struct SessionInfo {
    std::string id;
    std::wstring title;  // the CLI's own title (custom, else the generated one); may be empty
    FILETIME modified{};
};

enum class EntryKind { User, Assistant, Tool };

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

struct Settings {
    std::string model;   // a value from the CLI's model list; empty: its default
    std::string effort;  // low … max; empty: the model's default
    // What runs without a card: "" asks before anything that changes something, "edits" writes files on this
    // computer unasked (commands and servers still ask), "all" asks nothing.
    std::string permissions;
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

std::vector<SessionInfo> ListSessions(const std::wstring& workspace);  // newest first
std::wstring SessionTitle(const std::wstring& workspace, const std::string& id);
// Moves a chat to the Recycle Bin: its transcript and the folder the CLI keeps beside it. False if it could not.
bool DeleteSession(const std::wstring& workspace, const std::string& id);
// Gives a chat its own title the way the CLI's /rename does: a custom-title entry appended to its file
// (the newest one wins, over the generated title too).
bool RenameSession(const std::wstring& workspace, const std::string& id, const std::wstring& title);
// The last `limit` entries of a chat, for showing it again before it is resumed.
std::vector<TranscriptEntry> LoadTranscript(const std::wstring& workspace, const std::string& id, size_t limit,
                                            bool& truncated);

Settings LoadSettings();
void SaveSettings(const Settings& settings);

}  // namespace aic
