#include "core/chat_export.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>
#include <vector>

#include "core/claude_process.h"
#include "core/tools.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

constexpr size_t kOutputLongest = 2000;  // a tool's output in the file: its start, in bytes

// A fence longer than any run of backticks in the text, so the text cannot close it.
std::string Fenced(const std::string& text) {
    size_t longest = 0;
    for (size_t at = 0, run = 0; at < text.size(); ++at) {
        run = text[at] == '`' ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    const std::string fence(std::max<size_t>(3, longest + 1), '`');
    return fence + "\n" + text + (text.ends_with('\n') ? "" : "\n") + fence + "\n";
}

// The start of a long output, cut between characters.
std::string Head(std::string text) {
    if (text.size() <= kOutputLongest) {
        return text;
    }
    size_t cut = kOutputLongest;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    const size_t rest = text.size() - cut;
    text.resize(cut);
    return text + std::format("\n… [ещё {} байт]", rest);
}

bool LocalTime(const FILETIME& time, SYSTEMTIME& local) {
    SYSTEMTIME utc{};
    return FileTimeToSystemTime(&time, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
}

}  // namespace

std::string ChatTime(const FILETIME& time) {
    SYSTEMTIME local{};
    if (!LocalTime(time, local)) {
        return {};
    }
    return std::format("{:02}.{:02}.{} {:02}:{:02}", local.wDay, local.wMonth, local.wYear, local.wHour, local.wMinute);
}

std::optional<std::string> ChatMarkdown(const std::wstring& workspace, const std::string& id) {
    const std::vector<SessionInfo> sessions = ListSessions(workspace);
    const auto session = std::ranges::find(sessions, id, &SessionInfo::id);
    if (session == sessions.end()) {
        return std::nullopt;
    }
    bool truncated = false;
    const std::vector<TranscriptEntry> entries = LoadTranscript(workspace, id, SIZE_MAX, truncated);
    const std::string agent = session->grok ? "Grok" : "Claude";
    std::string out = "# " + ToUtf8(session->title.empty() ? L"Без названия" : session->title) + "\n\n";
    out += std::format("{} · {} · `{}`\n", agent, ChatTime(session->modified), ToUtf8(workspace));
    std::optional<bool> heading;  // whose turn the last heading opened: the user's (true) or the agent's
    for (const TranscriptEntry& entry : entries) {
        const bool user = entry.kind == EntryKind::User;
        if (entry.kind != EntryKind::Compacted && heading != user) {
            out += "\n## " + (user ? std::string("Пользователь") : agent) + "\n";
            heading = user;
        }
        switch (entry.kind) {
        case EntryKind::User:
        case EntryKind::Assistant:
            if (!entry.text.empty()) {
                out += "\n" + ToUtf8(entry.text) + "\n";
            }
            if (!entry.images.empty()) {
                out += std::format("\n*[картинок: {}]*\n", entry.images.size());
            }
            break;
        case EntryKind::Compacted:
            out += "\n---\n\n*Чат сжат: дальше " + agent + " помнил начало по сводке.*\n";
            heading.reset();
            break;
        case EntryKind::Tool: {
            const std::string own = OwnToolName(ToUtf8(entry.text));
            const Json input = Json::parse(entry.toolInput, nullptr, false);
            const ToolSummary summary =
                !own.empty() && input.is_object() ? SummarizeTool(own, input) : ToolSummary{entry.text, {}};
            out += "\n<details><summary>🔧 " + ToUtf8(summary.title) + "</summary>\n\n";
            if (!summary.detail.empty()) {
                out += Fenced(ToUtf8(summary.detail));
            }
            if (entry.toolAnswered && !entry.toolOutput.empty()) {
                out += std::string(entry.toolFailed ? "\nОшибка:\n\n" : "\nВывод:\n\n") +
                       Fenced(Head(ToUtf8(entry.toolOutput)));
            }
            out += "\n</details>\n";
            break;
        }
        }
    }
    return out;
}

std::wstring ChatExportPath(const SessionInfo& session) {
    fs::path folder;
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
        folder = documents;
    }
    CoTaskMemFree(documents);
    // The title as a file name: what Windows does not take becomes a space, at most 80 characters, no dot at the end.
    std::wstring name;
    for (const wchar_t c : session.title) {
        const bool bad = c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos;
        if (!(bad || c == L' ') || (!name.empty() && name.back() != L' ')) {
            name += bad ? L' ' : c;
        }
    }
    constexpr size_t kLongest = 80;
    if (name.size() > kLongest) {
        name.resize(IS_HIGH_SURROGATE(name[kLongest - 1]) ? kLongest - 1 : kLongest);
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.' || name.back() == L'…')) {
        name.pop_back();
    }
    if (name.empty()) {
        name = L"Чат";
    }
    SYSTEMTIME local{};
    if (LocalTime(session.modified, local)) {
        name += std::format(L" {}-{:02}-{:02}", local.wYear, local.wMonth, local.wDay);
    }
    return (folder / L"AIControls" / L"Чаты" / (name + L".md")).wstring();
}

}  // namespace aic
