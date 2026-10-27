#include "core/store.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <utility>

#include "core/claude_process.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) {
        return {};
    }
    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

// The CLI's folder name for a workspace: every character but [A-Za-z0-9] becomes '-'.
fs::path ProjectDir(const std::wstring& workspace) {
    std::wstring name = workspace;
    for (wchar_t& c : name) {
        const bool keep = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
        if (!keep) {
            c = L'-';
        }
    }
    return fs::path(KnownFolder(FOLDERID_Profile)) / L".claude" / L"projects" / name;
}

fs::path SessionFile(const std::wstring& workspace, const std::string& id) {
    return ProjectDir(workspace) / (ToWide(id) + L".jsonl");
}

std::string ReadRange(const fs::path& file, uint64_t offset, uint64_t size) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(static_cast<std::streamoff>(offset));
    std::string data(static_cast<size_t>(size), '\0');
    in.read(data.data(), static_cast<std::streamsize>(size));
    data.resize(static_cast<size_t>(in.gcount()));
    return data;
}

template <typename Fn>
void ForEachLine(std::string_view text, Fn&& fn) {
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        if (end > start) {
            fn(text.substr(start, end - start));
        }
        start = end + 1;
    }
}

std::wstring TextOf(const Json& content, bool images = true);  // images: a line for each picture

// A chat the CLI never titled is named after its first message: its first line, shortened.
std::wstring FirstPrompt(std::string_view text) {
    std::wstring found;
    ForEachLine(text, [&](std::string_view line) {
        if (!found.empty() || line.find("\"type\":\"user\"") == std::string_view::npos) {
            return;
        }
        const Json entry = Json::parse(line, nullptr, false);
        if (JsonStr(entry, "type") != "user" || (JsonAt(entry, "isMeta").is_boolean() && JsonAt(entry, "isMeta").get<bool>())) {
            return;
        }
        std::wstring prompt = TextOf(JsonAt(JsonAt(entry, "message"), "content"));
        const size_t first = prompt.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos || prompt[first] == L'<') {  // tool results, command and system insertions
            return;
        }
        // The title is the first line of the words; a picture or file mark (an attachment's line) only if there are
        // none, and then without the mark, which a font may draw as a squiggle.
        const std::wstring_view mark = L"📎 ";
        std::wstring titled;
        std::wstring attached;
        for (size_t at = first; at < prompt.size();) {
            const size_t end = std::min(prompt.find_first_of(L"\r\n", at), prompt.size());
            const std::wstring_view row = std::wstring_view(prompt).substr(at, end - at);
            if (row.starts_with(mark)) {
                attached = attached.empty() ? std::wstring(row.substr(mark.size())) : attached;
            } else if (!row.empty()) {
                titled = row;
                break;
            }
            at = end + 1;
        }
        prompt = titled.empty() ? attached : titled;
        if (prompt.empty()) {
            return;
        }
        constexpr size_t kLongest = 70;
        if (prompt.size() > kLongest) {
            prompt = prompt.substr(0, kLongest) + L"…";
        }
        found = std::move(prompt);
    });
    return found;
}

void ScanTitles(std::string_view text, std::wstring& custom, std::wstring& generated) {
    ForEachLine(text, [&](std::string_view line) {
        if (line.find("\"custom-title\"") == std::string_view::npos &&
            line.find("\"ai-title\"") == std::string_view::npos) {
            return;
        }
        const Json entry = Json::parse(line, nullptr, false);  // a line cut by a chunk edge fails: skipped
        const std::string type = JsonStr(entry, "type");
        if (type == "custom-title" && !JsonStr(entry, "customTitle").empty()) {
            custom = ToWide(JsonStr(entry, "customTitle"));
        } else if (type == "ai-title" && !JsonStr(entry, "aiTitle").empty()) {
            generated = ToWide(JsonStr(entry, "aiTitle"));
        }
    });
}

// Titles are appended as the chat goes on, so the latest is near the end; long chats are read
// only at the head and the tail.
std::wstring ReadTitle(const fs::path& file, uint64_t size) {
    constexpr uint64_t kChunk = 512 * 1024;
    std::wstring custom;
    std::wstring generated;
    const std::string head = ReadRange(file, 0, std::min(size, 2 * kChunk));
    if (size <= 2 * kChunk) {
        ScanTitles(head, custom, generated);
    } else {
        ScanTitles(std::string_view(head).substr(0, kChunk), custom, generated);
        ScanTitles(ReadRange(file, size - kChunk, kChunk), custom, generated);
    }
    if (!custom.empty()) {
        return custom;
    }
    return !generated.empty() ? generated : FirstPrompt(head);
}

uint64_t ToTicks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

struct CachedTitle {
    uint64_t modified = 0;
    uint64_t size = 0;
    std::wstring title;
};

// UI thread only.
std::wstring CachedReadTitle(const fs::path& file, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    static std::map<std::wstring, CachedTitle> cache;
    const uint64_t modified = ToTicks(data.ftLastWriteTime);
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    CachedTitle& entry = cache[file.wstring()];
    if (entry.modified != modified || entry.size != size) {
        entry = {modified, size, ReadTitle(file, size)};
    }
    return entry.title;
}

std::wstring TextOf(const Json& content, bool images) {
    if (content.is_string()) {
        return ToWide(content.get<std::string>());
    }
    std::wstring text;
    if (content.is_array()) {
        for (const Json& block : content) {
            const std::string type = JsonStr(block, "type");
            std::wstring part;
            if (type == "text" && !JsonStr(block, "text").starts_with("<earlier_chat>")) {  // a switch's handoff
                part = ToWide(JsonStr(block, "text"));
            } else if (type == "image" && images) {
                part = L"📎 фото";
            } else if (type == "document") {
                part = L"📎 " + (JsonStr(block, "title").empty() ? L"документ" : ToWide(JsonStr(block, "title")));
            }
            if (!part.empty()) {
                text += (text.empty() ? L"" : L"\n\n") + part;
            }
        }
    }
    return text;
}

bool IsTrue(const Json& value) {
    return value.is_boolean() && value.get<bool>();
}

fs::path SettingsFile() {
    return fs::path(KnownFolder(FOLDERID_RoamingAppData)) / L"AIControls" / L"settings.json";
}

}  // namespace

std::wstring DefaultWorkspace() {
    const fs::path old = fs::path(KnownFolder(FOLDERID_RoamingAppData)) / L"ssh-desktop-app" / L"claude-workspace";
    std::error_code error;
    if (fs::is_directory(old, error)) {
        return old.wstring();
    }
    return KnownFolder(FOLDERID_Profile);
}

std::wstring MemoryFolder(const std::wstring& workspace) {
    return (ProjectDir(workspace) / L"memory").wstring();
}

std::wstring SharedMemoryFolder() {
    return MemoryFolder(DefaultWorkspace());
}

std::vector<SessionInfo> ListSessions(const std::wstring& workspace) {
    std::vector<SessionInfo> sessions;
    std::error_code error;
    for (const fs::directory_entry& item : fs::directory_iterator(ProjectDir(workspace), error)) {
        if (!item.is_regular_file(error) || item.path().extension() != L".jsonl") {
            continue;
        }
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(item.path().c_str(), GetFileExInfoStandard, &data)) {
            continue;
        }
        SessionInfo session;
        session.id = ToUtf8(item.path().stem().wstring());
        session.title = CachedReadTitle(item.path(), data);
        session.modified = data.ftLastWriteTime;
        sessions.push_back(std::move(session));
    }
    std::ranges::sort(sessions, [](const SessionInfo& a, const SessionInfo& b) {
        return ToTicks(a.modified) > ToTicks(b.modified);
    });
    return sessions;
}

bool DeleteSession(const std::wstring& workspace, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos) {
        return false;  // an id is a plain name; anything else could reach outside the chats' folder
    }
    const fs::path file = SessionFile(workspace, id);
    const fs::path folder = ProjectDir(workspace) / ToWide(id);
    std::error_code error;
    std::wstring paths;  // SHFileOperation's list: each path ends in \0, the list in one more
    for (const fs::path& path : {file, folder}) {
        if (fs::exists(path, error)) {
            paths += path.wstring();
            paths += L'\0';
        }
    }
    if (paths.empty()) {
        return false;
    }
    paths += L'\0';
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = paths.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted && !fs::exists(file, error);
}

bool MoveSession(const std::wstring& from, const std::wstring& to, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos || ProjectDir(from) == ProjectDir(to)) {
        return false;
    }
    const fs::path file = SessionFile(from, id);
    const fs::path target = SessionFile(to, id);
    std::error_code error;
    if (!fs::exists(file, error) || fs::exists(target, error)) {
        return false;
    }
    fs::create_directories(ProjectDir(to), error);
    fs::rename(file, target, error);
    if (error) {
        return false;
    }
    // The folder the CLI keeps beside it (tool results and the like); the chat goes on without it if it stays.
    const fs::path folder = ProjectDir(from) / ToWide(id);
    if (fs::is_directory(folder, error) && !fs::exists(ProjectDir(to) / ToWide(id), error)) {
        fs::rename(folder, ProjectDir(to) / ToWide(id), error);
    }
    return true;
}

bool RenameSession(const std::wstring& workspace, const std::string& id, const std::wstring& title) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos || title.empty()) {
        return false;
    }
    const fs::path file = SessionFile(workspace, id);
    std::error_code error;
    if (!fs::exists(file, error)) {
        return false;
    }
    std::string line = Json{{"type", "custom-title"}, {"customTitle", ToUtf8(title)}, {"sessionId", id}}.dump(
        -1, ' ', false, Json::error_handler_t::replace);
    line += '\n';
    std::ofstream out(file, std::ios::binary | std::ios::app);
    out.write(line.data(), static_cast<std::streamsize>(line.size()));  // one write: the CLI may append too
    return out.good();
}

std::wstring SessionTitle(const std::wstring& workspace, const std::string& id) {
    const fs::path file = SessionFile(workspace, id);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        return {};
    }
    return CachedReadTitle(file, data);
}

std::vector<TranscriptEntry> LoadTranscript(const std::wstring& workspace, const std::string& id, size_t limit,
                                            bool& truncated) {
    std::vector<TranscriptEntry> entries;
    const fs::path file = SessionFile(workspace, id);
    std::error_code error;
    const uint64_t size = fs::file_size(file, error);
    if (error) {
        truncated = false;
        return entries;
    }
    std::string lastAssistant;  // one API message arrives as a line per content block
    ForEachLine(ReadRange(file, 0, size), [&](std::string_view line) {
        const Json entry = Json::parse(line, nullptr, false);
        if (entry.is_discarded() || IsTrue(JsonAt(entry, "isSidechain")) || IsTrue(JsonAt(entry, "isMeta"))) {
            return;
        }
        const std::string type = JsonStr(entry, "type");
        const Json& message = JsonAt(entry, "message");
        if (type == "system" && JsonStr(entry, "subtype") == "compact_boundary") {
            lastAssistant.clear();
            entries.push_back({EntryKind::Compacted});
            return;
        }
        if (type == "user" && IsTrue(JsonAt(entry, "isCompactSummary"))) {
            return;  // the summary the CLI wrote for itself, not the user's words
        }
        if (type == "user") {
            lastAssistant.clear();
            const Json& content = JsonAt(message, "content");
            std::wstring text = TextOf(content, false);
            std::vector<TranscriptImage> images;
            if (content.is_array()) {
                for (const Json& block : content) {
                    if (JsonStr(block, "type") == "tool_result") {  // the answer to a call listed above
                        const std::string id = JsonStr(block, "tool_use_id");
                        for (auto call = entries.rbegin(); call != entries.rend(); ++call) {
                            if (call->kind != EntryKind::Tool || call->toolId != id) {
                                continue;
                            }
                            const Json& result = JsonAt(block, "content");
                            std::wstring output;
                            if (result.is_string()) {
                                output = ToWide(result.get<std::string>());
                            } else if (result.is_array()) {
                                for (const Json& part : result) {
                                    if (JsonStr(part, "type") == "text") {
                                        output += (output.empty() ? L"" : L"\n") + ToWide(JsonStr(part, "text"));
                                    }
                                }
                            }
                            const size_t end = output.find_last_not_of(L" \t\r\n");
                            output.resize(end == std::wstring::npos ? 0 : end + 1);
                            call->toolOutput = std::move(output);
                            call->toolFailed = IsTrue(JsonAt(block, "is_error"));
                            call->toolAnswered = true;
                            break;
                        }
                    }
                    const Json& source = JsonAt(block, "source");
                    if (JsonStr(block, "type") == "image" && JsonStr(source, "type") == "base64") {
                        images.push_back({JsonStr(source, "media_type"), JsonStr(source, "data")});
                    }
                }
            }
            // Tool results carry no text; "<command-…>" and the like are the CLI's own bookkeeping.
            if ((!text.empty() && text.front() != L'<') || (text.empty() && !images.empty())) {
                entries.push_back({EntryKind::User, std::move(text), std::move(images)});
            }
        } else if (type == "assistant") {
            const std::string messageId = JsonStr(message, "id");
            const Json& content = JsonAt(message, "content");
            if (!content.is_array()) {
                return;
            }
            for (const Json& block : content) {
                const std::string blockType = JsonStr(block, "type");
                if (blockType == "text") {
                    std::wstring text = ToWide(JsonStr(block, "text"));
                    if (text.empty()) {
                        continue;
                    }
                    if (!entries.empty() && entries.back().kind == EntryKind::Assistant &&
                        lastAssistant == messageId && !messageId.empty()) {
                        entries.back().text += L"\n\n" + text;
                    } else {
                        entries.push_back({EntryKind::Assistant, std::move(text)});
                    }
                    lastAssistant = messageId;
                } else if (blockType == "tool_use") {
                    TranscriptEntry call{EntryKind::Tool, ToWide(JsonStr(block, "name"))};
                    call.toolId = JsonStr(block, "id");
                    call.toolInput = JsonAt(block, "input").dump();
                    entries.push_back(std::move(call));
                    lastAssistant.clear();
                }
            }
        }
    });
    truncated = entries.size() > limit;
    if (truncated) {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
    }
    return entries;
}

Settings LoadSettings() {
    Settings settings;
    std::ifstream in(SettingsFile(), std::ios::binary);
    if (!in) {
        return settings;
    }
    const Json data = Json::parse(in, nullptr, false);
    settings.model = JsonStr(data, "model");
    settings.effort = JsonStr(data, "effort");
    settings.permissions = JsonStr(data, "permissions");
    settings.claudeModels = JsonStr(data, "claudeModels");
    settings.connectors = JsonAt(data, "connectors").is_boolean() && JsonAt(data, "connectors").get<bool>();
    settings.grokFast = JsonAt(data, "grokFast").is_boolean() && JsonAt(data, "grokFast").get<bool>();
    const auto number = [&](const char* key, long long fallback) {
        const Json& value = JsonAt(data, key);
        return value.is_number() ? value.get<long long>() : fallback;
    };
    settings.sessionPercent = static_cast<int>(number("sessionPercent", -1));
    settings.sessionResets = number("sessionResets", 0);
    settings.weekPercent = static_cast<int>(number("weekPercent", -1));
    settings.weekResets = number("weekResets", 0);
    const auto real = [&](const char* key) {
        const Json& value = JsonAt(data, key);
        return value.is_number() ? value.get<float>() : 0.f;
    };
    settings.sidebarWidth = real("sidebarWidth");
    settings.terminalWidth = real("terminalWidth");
    settings.workspace = ToWide(JsonStr(data, "workspace"));
    settings.reopen = JsonStr(data, "reopen");
    const auto folders = [&](const char* key) {
        std::vector<std::wstring> list;
        if (const Json& value = JsonAt(data, key); value.is_array()) {
            for (const Json& folder : value) {
                if (folder.is_string()) {
                    list.push_back(ToWide(folder.get<std::string>()));
                }
            }
        }
        return list;
    };
    // Before projects there were the recent folders (newest first): they become the projects, oldest first.
    settings.projects = JsonAt(data, "projects").is_array() ? folders("projects") : folders("recentFolders");
    if (!JsonAt(data, "projects").is_array()) {
        std::ranges::reverse(settings.projects);
    }
    settings.closedProjects = folders("closedProjects");
    return settings;
}

namespace {
bool g_settingsReadOnly = false;
}  // namespace

void SetSettingsReadOnly(bool readOnly) {
    g_settingsReadOnly = readOnly;
}

void SaveSettings(const Settings& settings) {
    if (g_settingsReadOnly) {
        return;
    }
    const fs::path file = SettingsFile();
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const auto folders = [](const std::vector<std::wstring>& list) {
        Json array = Json::array();
        for (const std::wstring& folder : list) {
            array.push_back(ToUtf8(folder));
        }
        return array;
    };
    out << Json{{"model", settings.model},
                {"effort", settings.effort},
                {"permissions", settings.permissions},
                {"claudeModels", settings.claudeModels},
                {"connectors", settings.connectors},
        {"grokFast", settings.grokFast},
                {"sessionPercent", settings.sessionPercent},
                {"sessionResets", settings.sessionResets},
                {"weekPercent", settings.weekPercent},
                {"weekResets", settings.weekResets},
                {"sidebarWidth", settings.sidebarWidth},
                {"terminalWidth", settings.terminalWidth},
                {"workspace", ToUtf8(settings.workspace)},
                {"projects", folders(settings.projects)},
                {"closedProjects", folders(settings.closedProjects)},
                {"reopen", settings.reopen}}
               .dump(2)
        << "\n";
}

}  // namespace aic
