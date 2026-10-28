#include "core/store.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "core/claude_process.h"
#include "core/tools.h"

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

// A message as a title: the first line of the words; a picture or file mark (an attachment's line) only if there are
// none, and then without the mark, which a font may draw as a squiggle. Shortened.
std::wstring PromptTitle(const std::wstring& prompt) {
    const std::wstring_view mark = L"📎 ";
    std::wstring titled;
    std::wstring attached;
    for (size_t at = std::min(prompt.find_first_not_of(L" \t\r\n"), prompt.size()); at < prompt.size();) {
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
    std::wstring title = titled.empty() ? attached : titled;
    constexpr size_t kLongest = 70;
    if (title.size() > kLongest) {
        title = title.substr(0, kLongest) + L"…";
    }
    return title;
}

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
        const std::wstring prompt = TextOf(JsonAt(JsonAt(entry, "message"), "content"));
        const size_t first = prompt.find_first_not_of(L" \t\r\n");
        if (first == std::wstring::npos || prompt[first] == L'<') {  // tool results, command and system insertions
            return;
        }
        found = PromptTitle(prompt);
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

// The titles' caches: the sidebar reads them, and so do list_chats and read_chat on their threads.
std::mutex g_titlesMutex;

std::wstring CachedReadTitle(const fs::path& file, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    static std::map<std::wstring, CachedTitle> cache;
    const std::lock_guard lock(g_titlesMutex);
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

// The last `limit` entries; `truncated` says there were more.
void KeepLatest(std::vector<TranscriptEntry>& entries, size_t limit, bool& truncated) {
    truncated = entries.size() > limit;
    if (truncated) {
        entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
    }
}

// ---- Grok's chats (grok 1.0.62, 27.10.2026): a folder each, its messages a line each in chat_history.jsonl:
// {"type":"user","content":[blocks],"prompt_index":N} what the user sent (without prompt_index: Grok's rules and
// reminders), {"type":"assistant","content":"…","tool_calls":[{id,name,arguments}]}, {"type":"tool_result",
// "tool_call_id":…,"content":"…"}.

constexpr const wchar_t* kGrokHistory = L"chat_history.jsonl";
constexpr const wchar_t* kGrokTitle = L"aicontrols-title.txt";  // a title given here; Grok's own files stay as they are

// The folder's name as Grok makes it: the path in UTF-8, all but [A-Za-z0-9-._~] as %XX.
std::wstring PercentEncode(const std::wstring& text) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::wstring encoded;
    for (const char c : ToUtf8(text)) {
        const auto byte = static_cast<unsigned char>(c);
        const bool keep = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
                          byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (keep) {
            encoded += static_cast<wchar_t>(byte);
        } else {
            encoded += L'%';
            encoded += static_cast<wchar_t>(kHex[byte >> 4]);
            encoded += static_cast<wchar_t>(kHex[byte & 0xF]);
        }
    }
    return encoded;
}

std::wstring PercentDecode(const std::wstring& name) {
    const auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    const std::string text = ToUtf8(name);
    std::string decoded;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            decoded += static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else {
            decoded += text[i];
        }
    }
    return ToWide(decoded);
}

fs::path GrokProjectDir(const std::wstring& workspace) {
    const fs::path root = fs::path(KnownFolder(FOLDERID_Profile)) / L".grok" / L"sessions";
    const fs::path named = root / PercentEncode(workspace);
    std::error_code error;
    if (fs::is_directory(named, error)) {
        return named;
    }
    for (const fs::directory_entry& item : fs::directory_iterator(root, error)) {  // the path written in another case
        const std::wstring path = PercentDecode(item.path().filename().wstring());
        if (CompareStringOrdinal(path.c_str(), -1, workspace.c_str(), -1, TRUE) == CSTR_EQUAL) {
            return item.path();
        }
    }
    return named;
}

fs::path GrokSessionDir(const std::wstring& workspace, const std::string& id) {
    if (id.empty() || id.find_first_of("/\\.:") != std::string::npos) {
        return {};  // an id is a plain name; anything else could reach outside the chats' folder
    }
    return GrokProjectDir(workspace) / ToWide(id);
}

bool IsGrokPrompt(const Json& entry) {
    return JsonStr(entry, "type") == "user" && JsonAt(entry, "prompt_index").is_number() &&
           !entry.contains("synthetic_reason");
}

// The words as they were typed: Grok puts them in <user_query>, after its list of the pictures it saved; a switch's
// handoff (<earlier_chat>) comes first among them.
std::wstring GrokPromptText(const Json& content) {
    std::string text = content.is_string() ? content.get<std::string>() : std::string();
    if (content.is_array()) {
        for (const Json& block : content) {
            if (JsonStr(block, "type") == "text") {
                text += JsonStr(block, "text");
            }
        }
    }
    constexpr std::string_view kOpen = "<user_query>";
    constexpr std::string_view kClose = "</user_query>";
    const size_t open = text.find(kOpen);
    const size_t close = text.rfind(kClose);
    if (open != std::string::npos && close != std::string::npos && close > open) {
        text = text.substr(open + kOpen.size(), close - open - kOpen.size());
    }
    constexpr std::string_view kHandoffEnd = "</earlier_chat>";
    if (const size_t start = text.find_first_not_of(" \t\r\n");
        start != std::string::npos && std::string_view(text).substr(start).starts_with("<earlier_chat>")) {
        const size_t end = text.find(kHandoffEnd);
        text.erase(0, end == std::string::npos ? text.size() : end + kHandoffEnd.size());
    }
    const size_t first = text.find_first_not_of(" \t\r\n");
    const size_t last = text.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::wstring() : ToWide(std::string_view(text).substr(first, last - first + 1));
}

// The pictures sent with it: {"type":"image","url":"data:image/png;base64,…"}.
std::vector<TranscriptImage> GrokPromptImages(const Json& content) {
    std::vector<TranscriptImage> images;
    if (!content.is_array()) {
        return images;
    }
    for (const Json& block : content) {
        if (JsonStr(block, "type") != "image") {
            continue;
        }
        const std::string url = JsonStr(block, "url");
        const size_t data = url.find(";base64,");
        if (url.starts_with("data:") && data != std::string::npos) {
            images.push_back({url.substr(5, data - 5), url.substr(data + 8)});
        }
    }
    return images;
}

// A Grok chat is named after its first message (Grok's own titles come in English). nullopt: nothing was sent, the
// session is a start of grok.exe and no chat. The lines before it are Grok's rules, a few.
std::optional<std::wstring> ReadGrokTitle(const fs::path& history) {
    std::ifstream in(history, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (line.find("\"prompt_index\"") == std::string::npos) {
            continue;
        }
        const Json entry = Json::parse(line, nullptr, false);
        if (!IsGrokPrompt(entry)) {
            continue;
        }
        const Json& content = JsonAt(entry, "content");
        std::wstring words = GrokPromptText(content);
        const bool pictures = content.is_array() && std::ranges::any_of(content, [](const Json& block) {
            return JsonStr(block, "type") == "image";
        });
        if (pictures) {
            words += (words.empty() ? L"" : L"\n\n") + std::wstring(L"📎 фото");
        }
        return PromptTitle(words);
    }
    return std::nullopt;
}

struct CachedGrokTitle {
    uint64_t modified = 0;
    uint64_t size = 0;
    std::optional<std::wstring> title;
};

std::optional<std::wstring> GrokTitle(const fs::path& history, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    static std::map<std::wstring, CachedGrokTitle> cache;
    const std::lock_guard lock(g_titlesMutex);
    const uint64_t modified = ToTicks(data.ftLastWriteTime);
    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    CachedGrokTitle& entry = cache[history.wstring()];
    if (entry.modified != modified || entry.size != size) {
        entry = {modified, size, ReadGrokTitle(history)};
    }
    return entry.title;
}

// The title given in the sidebar, if one was.
std::wstring GrokOwnTitle(const fs::path& folder) {
    std::ifstream in(folder / kGrokTitle, std::ios::binary);
    if (!in) {
        return {};
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return ToWide(text);
}

std::vector<TranscriptEntry> LoadGrokTranscript(const fs::path& history) {
    std::vector<TranscriptEntry> entries;
    std::ifstream in(history, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        const Json entry = Json::parse(line, nullptr, false);
        const std::string type = JsonStr(entry, "type");
        if (type == "user" && IsGrokPrompt(entry)) {
            const Json& content = JsonAt(entry, "content");
            TranscriptEntry said{EntryKind::User, GrokPromptText(content), GrokPromptImages(content)};
            if (!said.text.empty() || !said.images.empty()) {
                entries.push_back(std::move(said));
            }
        } else if (type == "assistant") {
            if (std::wstring text = ToWide(JsonStr(entry, "content")); !text.empty()) {
                entries.push_back({EntryKind::Assistant, std::move(text)});
            }
            const Json& calls = JsonAt(entry, "tool_calls");
            if (!calls.is_array()) {
                continue;
            }
            for (const Json& call : calls) {
                std::string name = JsonStr(call, "name");
                if (name == "search_tool") {
                    continue;  // Grok looking up our tools' descriptions: nothing was done
                }
                Json input = Json::parse(JsonStr(call, "arguments"), nullptr, false);
                if (name == "use_tool" && JsonAt(input, "tool_input").is_object()) {
                    name = "mcp__" + JsonStr(input, "tool_name");  // as it comes live: aic__read_file -> mcp__aic__read_file
                    Json inner = JsonAt(input, "tool_input");
                    input = std::move(inner);
                } else if (GrokOwnTool(name)) {
                    name = kGrokToolPrefix + name;  // image_gen -> grok__image_gen
                }
                TranscriptEntry tool{EntryKind::Tool, ToWide(name)};
                tool.toolId = JsonStr(call, "id");
                if (!input.is_discarded()) {
                    tool.toolInput = input.dump(-1, ' ', false, Json::error_handler_t::replace);
                }
                entries.push_back(std::move(tool));
            }
        } else if (type == "tool_result") {
            const std::string id = JsonStr(entry, "tool_call_id");
            for (auto call = entries.rbegin(); call != entries.rend(); ++call) {
                if (call->kind != EntryKind::Tool || call->toolId != id) {
                    continue;
                }
                std::wstring output = ToWide(JsonStr(entry, "content"));
                const size_t end = output.find_last_not_of(L" \t\r\n");
                output.resize(end == std::wstring::npos ? 0 : end + 1);
                // How Grok keeps a call that failed: "Tool `image_gen` failed: ...".
                call->toolFailed = output.starts_with(L"Tool `") && output.find(L"` failed: ") != std::wstring::npos;
                call->toolOutput = std::move(output);
                call->toolAnswered = true;
                break;
            }
        } else if (type == "backend_tool_call") {  // a web search, done on xAI's side
            const Json& kind = JsonAt(entry, "kind");
            if (JsonStr(kind, "tool_type") == "web_search") {
                TranscriptEntry search{EntryKind::Tool, ToWide(std::string(kGrokToolPrefix) + "web_search")};
                search.toolInput = JsonAt(kind, "action").dump(-1, ' ', false, Json::error_handler_t::replace);
                search.toolAnswered = true;
                entries.push_back(std::move(search));
            }
        }
    }
    return entries;
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
    for (const fs::directory_entry& item : fs::directory_iterator(GrokProjectDir(workspace), error)) {
        const fs::path history = item.path() / kGrokHistory;
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!item.is_directory(error) || !GetFileAttributesExW(history.c_str(), GetFileExInfoStandard, &data)) {
            continue;
        }
        const std::optional<std::wstring> prompt = GrokTitle(history, data);
        if (!prompt) {
            continue;
        }
        SessionInfo session;
        session.id = ToUtf8(item.path().filename().wstring());
        session.title = GrokOwnTitle(item.path());
        if (session.title.empty()) {
            session.title = *prompt;
        }
        session.modified = data.ftLastWriteTime;
        session.grok = true;
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
    const fs::path grok = GrokSessionDir(workspace, id);
    std::error_code error;
    std::wstring paths;  // SHFileOperation's list: each path ends in \0, the list in one more
    for (const fs::path& path : {file, folder, grok}) {
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
    return SHFileOperationW(&operation) == 0 && !operation.fAnyOperationsAborted && !fs::exists(file, error) &&
           !fs::exists(grok, error);
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
        const fs::path grok = GrokSessionDir(workspace, id);
        if (!fs::exists(grok / kGrokHistory, error)) {
            return false;
        }
        const std::string text = ToUtf8(title);
        std::ofstream out(grok / kGrokTitle, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        return out.good();
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
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) {
        return CachedReadTitle(file, data);
    }
    const fs::path grok = GrokSessionDir(workspace, id);
    if (grok.empty() || !GetFileAttributesExW((grok / kGrokHistory).c_str(), GetFileExInfoStandard, &data)) {
        return {};
    }
    std::wstring title = GrokOwnTitle(grok);
    return !title.empty() ? title : GrokTitle(grok / kGrokHistory, data).value_or(std::wstring());
}

std::wstring GrokHistoryFolder(const std::wstring& workspace) {
    return GrokProjectDir(workspace).wstring();
}

std::vector<TranscriptEntry> LoadTranscript(const std::wstring& workspace, const std::string& id, size_t limit,
                                            bool& truncated) {
    std::vector<TranscriptEntry> entries;
    const fs::path file = SessionFile(workspace, id);
    std::error_code error;
    const uint64_t size = fs::file_size(file, error);
    if (error) {
        if (const fs::path grok = GrokSessionDir(workspace, id); !grok.empty()) {
            entries = LoadGrokTranscript(grok / kGrokHistory);
        }
        KeepLatest(entries, limit, truncated);
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
    KeepLatest(entries, limit, truncated);
    return entries;
}

std::vector<ProjectPlace> OrderProjects(std::vector<ProjectPlace> projects, const std::wstring& general,
                                        const std::wstring& open, ProjectOrder order) {
    const size_t count = projects.size();
    std::vector<size_t> index(count);
    for (size_t i = 0; i < count; ++i) {
        index[i] = i;
    }
    std::ranges::stable_sort(index, [&](size_t a, size_t b) {
        const bool aGeneral = !general.empty() && projects[a].folder == general;
        const bool bGeneral = !general.empty() && projects[b].folder == general;
        if (aGeneral != bGeneral) {
            return bGeneral;  // the general chats stay a section under the projects
        }
        if (aGeneral) {
            return false;
        }
        if (order == ProjectOrder::Name) {
            const int compared = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, projects[a].name.c_str(), -1,
                                                 projects[b].name.c_str(), -1);
            return compared == CSTR_LESS_THAN;
        }
        if (order == ProjectOrder::Recent) {
            const bool aOpen = !open.empty() && projects[a].folder == open;
            const bool bOpen = !open.empty() && projects[b].folder == open;
            if (aOpen != bOpen) {
                return aOpen;  // the project on screen stays where the eye already is
            }
            if (projects[a].activity != projects[b].activity) {
                return projects[a].activity > projects[b].activity;
            }
        }
        return false;  // Added, and any tie: the order they were given
    });
    std::vector<ProjectPlace> ordered;
    ordered.reserve(count);
    for (const size_t i : index) {
        ordered.push_back(std::move(projects[i]));
    }
    return ordered;
}

ProjectOrder ParseProjectOrder(const std::string& text) {
    if (text == "added") {
        return ProjectOrder::Added;
    }
    if (text == "name") {
        return ProjectOrder::Name;
    }
    return ProjectOrder::Recent;
}

const char* ProjectOrderId(ProjectOrder order) {
    switch (order) {
    case ProjectOrder::Added:
        return "added";
    case ProjectOrder::Name:
        return "name";
    case ProjectOrder::Recent:
        return "recent";
    }
    return "recent";
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
    settings.grokPermissions = JsonStr(data, "grokPermissions");
    settings.claudeModels = JsonStr(data, "claudeModels");
    settings.claudeModel = JsonStr(data, "claudeModel");
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
    settings.projectOrder = ParseProjectOrder(JsonStr(data, "projectOrder"));
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
                {"grokPermissions", settings.grokPermissions},
                {"claudeModels", settings.claudeModels},
                {"claudeModel", settings.claudeModel},
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
                {"projectOrder", ProjectOrderId(settings.projectOrder)},
                {"reopen", settings.reopen}}
               .dump(2)
        << "\n";
}

}  // namespace aic
