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
};

struct Settings {
    std::string model;   // a value from the CLI's model list; empty: its default
    std::string effort;  // low … max; empty: the model's default
    // The plan's limits as last reported (the CLI reports them only once a turn has run), so the ring
    // is not empty at start. Percent -1: never seen; resets are Unix seconds.
    int sessionPercent = -1;
    long long sessionResets = 0;
    int weekPercent = -1;
    long long weekResets = 0;
    // The side panels' widths as the user dragged them, in DIPs; 0: the default.
    float sidebarWidth = 0;
    float terminalWidth = 0;
    // The folder the chats work in (empty: DefaultWorkspace) and the ones used before, newest first.
    std::wstring workspace;
    std::vector<std::wstring> recentFolders;
    // A chat to open again at the next start (set by an update, which restarts the client); cleared once opened.
    std::string reopen;
};

// The old SSH Client's workspace while both clients live side by side: Claude's memory and the
// chat history are keyed by this folder. Falls back to the profile folder.
std::wstring DefaultWorkspace();

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
