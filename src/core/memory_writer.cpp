#include "core/memory_writer.h"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include "core/auth.h"
#include "core/claude_process.h"
#include "core/store.h"

namespace aic {
namespace {

namespace fs = std::filesystem;

constexpr size_t kChatChars = 40'000;  // the end of the chat that is sent (~12k tokens)
constexpr size_t kEntryChars = 1'500;  // one message at most

std::string ReadText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// "description: ..." from a note's front matter.
std::string Description(const std::string& note) {
    static const std::regex line(R"((?:^|\n)description:\s*([^\r\n]*))");
    std::smatch match;
    return std::regex_search(note, match, line) ? match[1].str() : std::string();
}

bool SafeFileName(const std::string& file) {
    static const std::regex name(R"([a-z0-9][a-z0-9_\-]{0,60}\.md)");
    return std::regex_match(file, name) && file != "memory.md";
}

// Never into the memory: tokens, keys, passwords.
bool LooksSecret(const std::string& text) {
    static const std::regex secret(
        R"(ghp_[A-Za-z0-9]{20,}|github_pat_|sk-[A-Za-z0-9_\-]{20,}|AKIA[0-9A-Z]{16}|PRIVATE KEY|password\s*[:=]\s*\S|пароль\s*[:=]\s*\S)",
        std::regex::icase);
    return std::regex_search(text, secret);
}

std::string Str(const Json& object, const char* key) {
    const Json& value = JsonAt(object, key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

std::string OneLine(std::string text) {
    std::ranges::replace(text, '\n', ' ');
    std::ranges::replace(text, '\r', ' ');
    return text;
}

}  // namespace

std::string MemoryPrompt(const std::wstring& memoryFolder, const std::string& chat) {
    std::string files;
    std::error_code error;
    for (const fs::directory_entry& entry : fs::directory_iterator(memoryFolder, error)) {
        const std::string name = ToUtf8(entry.path().filename().wstring());
        if (entry.path().extension() == L".md" && name != "MEMORY.md") {
            files += "- " + name + " — " + Description(ReadText(entry.path())) + "\n";
        }
    }
    return "Это переписка пользователя с ассистентом. Реши, что из неё стоит сохранить в долговременную память "
           "ассистента: решения пользователя, его предпочтения и поправки, состояние проектов, что сделано и что "
           "осталось, где что лежит. Не сохраняй то, что видно из кода и git, мелочи этой беседы и любые пароли, "
           "токены и ключи (только имена секретов). Если подходящая заметка уже есть — дополни её, новую не заводи. "
           "Даты пиши абсолютные. Пиши по-русски, коротко.\n\n"
           "Заметки в памяти (файл — описание):\n" +
           (files.empty() ? std::string("(пока нет)\n") : files) +
           "\nОтветь ТОЛЬКО JSON, без пояснений:\n"
           "{\"append\":[{\"file\":\"<имя существующего файла>\",\"text\":\"<что дописать в конец>\"}],"
           "\"new\":[{\"file\":\"<type>_<slug>.md\",\"name\":\"<slug>\",\"description\":\"<одна строка>\","
           "\"type\":\"user|feedback|project|reference\",\"body\":\"<текст; для feedback/project — строки **Why:** и "
           "**How to apply:**>\",\"index\":\"- [<Заголовок>](<файл>) — <зачем>\"}]}\n"
           "Нечего сохранять — {\"append\":[],\"new\":[]}.\n\n"
           "Переписка:\n" +
           chat;
}

MemoryEdits ParseMemoryEdits(const std::string& answer) {
    MemoryEdits edits;
    const size_t first = answer.find('{');
    const size_t last = answer.rfind('}');
    if (first == std::string::npos || last == std::string::npos || last < first) {
        return edits;
    }
    const Json data = Json::parse(answer.substr(first, last - first + 1), nullptr, false);
    if (data.is_discarded()) {
        return edits;
    }
    if (const Json& appends = JsonAt(data, "append"); appends.is_array()) {
        for (const Json& item : appends) {
            MemoryAppend append{Str(item, "file"), Str(item, "text")};
            if (SafeFileName(append.file) && !append.text.empty() && !LooksSecret(append.text)) {
                edits.appends.push_back(std::move(append));
            }
        }
    }
    if (const Json& notes = JsonAt(data, "new"); notes.is_array()) {
        for (const Json& item : notes) {
            MemoryNote note{Str(item, "file"), Str(item, "name"), OneLine(Str(item, "description")),
                            Str(item, "type"), Str(item, "body"), OneLine(Str(item, "index"))};
            static const std::vector<std::string> kTypes = {"user", "feedback", "project", "reference"};
            if (std::ranges::find(kTypes, note.type) == kTypes.end()) {
                note.type = "project";
            }
            if (note.name.empty()) {
                note.name = note.file.substr(0, note.file.size() - 3);
            }
            if (SafeFileName(note.file) && !note.body.empty() &&
                !LooksSecret(note.body + note.description + note.index)) {
                edits.notes.push_back(std::move(note));
            }
        }
    }
    return edits;
}

void ApplyMemoryEdits(const std::wstring& memoryFolder, const MemoryEdits& edits, int& created, int& appended) {
    created = appended = 0;
    const fs::path folder(memoryFolder);
    std::error_code error;
    fs::create_directories(folder, error);
    const auto append = [](const fs::path& file, const std::string& text) {
        std::string existing = ReadText(file);
        std::ofstream out(file, std::ios::binary | std::ios::app);
        if (!existing.empty() && existing.back() != '\n') {
            out << '\n';
        }
        out << text << (text.ends_with('\n') ? "" : "\n");
        return out.good();
    };
    for (const MemoryAppend& item : edits.appends) {
        const fs::path file = folder / ToWide(item.file);
        if (fs::exists(file, error) && append(file, "\n" + item.text)) {
            ++appended;
        }
    }
    for (const MemoryNote& note : edits.notes) {
        const fs::path file = folder / ToWide(note.file);
        if (fs::exists(file, error)) {  // it is there after all: what is new goes to its end
            if (append(file, "\n" + note.body)) {
                ++appended;
            }
            continue;
        }
        std::ofstream out(file, std::ios::binary);
        out << "---\nname: " << note.name << "\ndescription: " << note.description << "\nmetadata:\n  type: "
            << note.type << "\n---\n\n" << note.body << (note.body.ends_with('\n') ? "" : "\n");
        if (!out.good()) {
            continue;
        }
        out.close();
        ++created;
        const std::string line = note.index.empty() ? "- [" + note.name + "](" + note.file + ") — " + note.description
                                                    : note.index;
        append(folder / L"MEMORY.md", line);
    }
}

MemoryWrite WriteMemoryFromChat(const std::wstring& workspace, const std::string& sessionId,
                                const std::atomic<bool>* cancel, MemoryEdits* preview) {
    MemoryWrite result;
    bool truncated = false;
    const std::vector<TranscriptEntry> entries = LoadTranscript(workspace, sessionId, 2000, truncated);
    // The chat's words from its end back, each message cut short: tool output never goes.
    std::vector<std::string> lines;
    size_t total = 0;
    for (auto it = entries.rbegin(); it != entries.rend() && total < kChatChars; ++it) {
        if (it->kind == EntryKind::Tool) {
            continue;
        }
        std::string text = ToUtf8(it->text);
        if (text.size() > kEntryChars) {
            text = text.substr(0, kEntryChars) + " […]";
        }
        lines.push_back((it->kind == EntryKind::User ? "Пользователь: " : "Ассистент: ") + text);
        total += lines.back().size();
    }
    if (lines.empty()) {
        result.ok = true;  // nothing said: nothing to remember
        return result;
    }
    std::string chat;
    if (workspace != DefaultWorkspace()) {  // the memory is every project's: what is learnt goes with this one
        chat = "(Чат шёл в проекте «" + ToUtf8(fs::path(workspace).filename().wstring()) + "», папка " +
               ToUtf8(workspace) + ".)\n\n";
    }
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        chat += *it + "\n\n";
    }
    const std::wstring folder = SharedMemoryFolder();

    // The prompt goes in through a file on standard input (too long for a command line), the call runs in a
    // scratch folder and keeps no session: nothing of it shows among the chats.
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const fs::path scratch = fs::path(temp) / L"aicontrols-memory";
    std::error_code error;
    fs::create_directories(scratch, error);
    const fs::path input = scratch / L"prompt.txt";
    {
        std::ofstream out(input, std::ios::binary);
        out << MemoryPrompt(folder, chat);
    }
    const std::wstring exe = FindClaudeExe();
    if (exe.empty()) {
        result.error = "claude.exe not found";
        return result;
    }
    const std::wstring command =
        L"\"" + exe + L"\" -p --model haiku --tools \"\" --strict-mcp-config --no-session-persistence "
        L"--output-format json --system-prompt \"You maintain an assistant's long-term memory notes. Answer with "
        L"JSON only.\"";
    const CliResult run = RunCli(command, L"", 300, cancel, {}, {}, scratch.wstring(), input.wstring());
    fs::remove(input, error);
    if (!run.started || run.timedOut || run.cancelled) {
        result.error = run.timedOut ? "timed out" : "did not run";
        return result;
    }
    // --output-format json: one object; its "result" is the model's text.
    const size_t brace = run.output.find('{');
    const Json reply = brace == std::string::npos ? Json() : Json::parse(run.output.substr(brace), nullptr, false);
    const std::string text = reply.is_object() ? Str(reply, "result") : std::string();
    if (run.exitCode != 0 || (reply.is_object() && JsonAt(reply, "is_error").is_boolean() &&
                              JsonAt(reply, "is_error").get<bool>())) {
        result.error = text.empty() ? run.output.substr(0, 400) : text.substr(0, 400);
        return result;
    }
    MemoryEdits edits = ParseMemoryEdits(text);
    if (preview != nullptr) {
        *preview = std::move(edits);
    } else {
        ApplyMemoryEdits(folder, edits, result.created, result.appended);
    }
    result.ok = true;
    return result;
}

}  // namespace aic
