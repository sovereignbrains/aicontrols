#include "core/tools.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <wil/resource.h>

namespace aic {
namespace {

namespace fs = std::filesystem;

constexpr size_t kReadLimit = 400'000;      // bytes of a file handed to the model
constexpr size_t kOutputKeepHead = 20'000;  // a long command output keeps its start ...
constexpr size_t kOutputKeepTail = 80'000;  // ... and, more of it, its end
constexpr size_t kOutputHardCap = 16u << 20;
constexpr size_t kListLimit = 500;
constexpr int kDefaultTimeout = 120;
constexpr int kMaxTimeout = 900;

ToolResult Error(std::string text) {
    return ToolResult{std::move(text), true};
}

std::string StringArg(const Json& input, const char* key) {
    return JsonStr(input, key);
}

bool BoolArg(const Json& input, const char* key) {
    const Json& value = JsonAt(input, key);
    return value.is_boolean() && value.get<bool>();
}

std::string Base64Bytes(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t size = bytes.size();
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const unsigned chunk = (static_cast<unsigned>(data[i]) << 16) |
                               (i + 1 < size ? static_cast<unsigned>(data[i + 1]) << 8 : 0u) |
                               (i + 2 < size ? static_cast<unsigned>(data[i + 2]) : 0u);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < size ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[chunk & 63] : '=';
    }
    return out;
}

// PowerShell's -EncodedCommand wants the script as UTF-16LE.
std::string Base64(const std::wstring& text) {
    return Base64Bytes(std::string_view(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t)));
}

bool ReadWhole(const fs::path& path, std::string& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool WriteWhole(const fs::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::string WithCrLf(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            out += '\r';
        }
        out += text[i];
    }
    return out;
}

size_t CountOccurrences(const std::string& text, const std::string& part) {
    size_t count = 0;
    for (size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) {
        ++count;
    }
    return count;
}

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
    std::string out;
    size_t pos = 0;
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, pos)) {
        out.append(text, pos, at - pos);
        out += to;
        pos = at + from.size();
    }
    out.append(text, pos, std::string::npos);
    return out;
}

// PowerShell's "#< CLIXML" error stream (<S S="Error">text</S> pieces with _x000D_ escapes) as plain text.
std::string DecodeClixml(const std::string& output) {
    const size_t marker = output.find("#< CLIXML");
    if (marker == std::string::npos) {
        return output;
    }
    std::string text = output.substr(0, marker);
    constexpr std::string_view open = "<S S=\"Error\">";
    for (size_t at = output.find(open, marker); at != std::string::npos; at = output.find(open, at)) {
        const size_t start = at + open.size();
        const size_t end = output.find("</S>", start);
        if (end == std::string::npos) {
            break;
        }
        std::string piece = output.substr(start, end - start);
        for (const auto& [from, to] : {std::pair<const char*, const char*>{"_x000D__x000A_", "\n"},
                                       {"_x000D_", ""}, {"_x000A_", "\n"}, {"&lt;", "<"}, {"&gt;", ">"},
                                       {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}}) {
            piece = ReplaceAll(std::move(piece), from, to);
        }
        text += piece;
        at = end + 4;
    }
    return text;
}

// A long output without its middle.
std::string Shorten(std::string text) {
    if (text.size() <= kOutputKeepHead + kOutputKeepTail) {
        return text;
    }
    const size_t cut = text.size() - kOutputKeepHead - kOutputKeepTail;
    return text.substr(0, kOutputKeepHead) + std::format("\n… [{} bytes cut from the middle] …\n", cut) +
           text.substr(text.size() - kOutputKeepTail);
}

std::string Truncated(const std::string& text, size_t limit) {
    return text.size() <= limit ? text : text.substr(0, limit) + "…";
}

Json Schema(Json properties, Json required) {
    return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)}};
}

// user@host[:port] as ssh takes it. Anything that could be read as an option or a shell word is refused.
struct Target {
    std::string host;  // user@host
    std::string port;  // empty: ssh's default
};

std::optional<Target> ParseTarget(std::string_view on) {
    if (on.empty() || on.size() > 255 || on.front() == '-') {
        return std::nullopt;
    }
    Target target;
    std::string_view host = on;
    if (const size_t colon = on.rfind(':'); colon != std::string_view::npos && on.find(':') == colon) {
        const std::string_view port = on.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string_view::npos) {
            return std::nullopt;
        }
        target.port = port;
        host = on.substr(0, colon);
    }
    const size_t at = host.find('@');
    if (host.empty() || host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-@") !=
                            std::string_view::npos ||
        (at != std::string_view::npos && (at == 0 || at + 1 == host.size() || host.find('@', at + 1) != std::string_view::npos ||
                                          host[at + 1] == '-'))) {
        return std::nullopt;
    }
    target.host = host;
    return target;
}

// A POSIX shell word for `text`: single-quoted, so nothing in it is interpreted.
std::string ShQuote(std::string_view text) {
    std::string out = "'";
    for (const char c : text) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
}

// The same for a path; a leading ~ still means the home folder.
std::string ShPath(std::string_view path) {
    if (path == "~") {
        return "\"$HOME\"";
    }
    if (path.starts_with("~/")) {
        return "\"$HOME\"" + ShQuote(path.substr(1));
    }
    return ShQuote(path);
}

enum class End { Exited, TimedOut, Stopped };

struct ProcessResult {
    std::string error;   // set when the process could not be started
    std::string output;  // stdout and stderr together, without a BOM
    DWORD code = 1;
    End end = End::Exited;
    long long ms = 0;
};

// Runs a command line to its end, to the timeout or to `cancel`, with `input` on its stdin (NUL when empty:
// a command that waits for input ends instead of hanging). Whatever it leaves running dies with it.
ProcessResult RunProcess(std::wstring commandLine, std::vector<wchar_t> environment, const std::wstring& dir,
                         int timeout, const std::string& input, const std::atomic<bool>& cancel) {
    ProcessResult result;
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    wil::unique_handle outRead, outWrite;
    if (!CreatePipe(outRead.put(), outWrite.put(), &inheritable, 1 << 16)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    wil::unique_handle inRead, inWrite;
    if (input.empty()) {
        inRead.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!inRead) {
            result.error = "Cannot open NUL";
            return result;
        }
    } else {
        if (!CreatePipe(inRead.put(), inWrite.put(), &inheritable, 1 << 16)) {
            result.error = "CreatePipe failed";
            return result;
        }
        SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0);
    }

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<std::byte> attributesBuffer(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        result.error = "InitializeProcThreadAttributeList failed";
        return result;
    }
    auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
    std::array<HANDLE, 2> inherited{inRead.get(), outWrite.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(inherited), nullptr, nullptr)) {
        result.error = "UpdateProcThreadAttribute failed";
        return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inRead.get();
    startup.StartupInfo.hStdOutput = outWrite.get();
    startup.StartupInfo.hStdError = outWrite.get();
    startup.lpAttributeList = attributes;

    wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        result.error = "CreateJobObject failed";
        return result;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const auto started = std::chrono::steady_clock::now();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), dir.empty() ? nullptr : dir.c_str(), &startup.StartupInfo, &info)) {
        result.error = std::format("Cannot start the process (error {}); cwd: {}", GetLastError(), ToUtf8(dir));
        return result;
    }
    wil::unique_handle process(info.hProcess);
    const wil::unique_handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        result.error = "AssignProcessToJobObject failed";
        return result;
    }
    ResumeThread(thread.get());
    outWrite.reset();  // ours; without closing it the reader never sees the end
    inRead.reset();

    std::string& output = result.output;
    std::thread reader([&] {
        std::array<char, 8192> chunk{};
        DWORD got = 0;
        while (::ReadFile(outRead.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
            if (output.size() < kOutputHardCap) {
                output.append(chunk.data(), got);
            }
        }
    });
    std::thread writer([&] {
        size_t sent = 0;
        while (inWrite && sent < input.size()) {  // ends with an error once the process is gone
            DWORD put = 0;
            if (!::WriteFile(inWrite.get(), input.data() + sent, static_cast<DWORD>(std::min<size_t>(input.size() - sent, 1 << 16)),
                             &put, nullptr) ||
                put == 0) {
                break;
            }
            sent += put;
        }
        inWrite.reset();  // the end of the input
    });

    const auto deadline = started + std::chrono::seconds(timeout);
    for (;;) {
        if (WaitForSingleObject(process.get(), 100) == WAIT_OBJECT_0) {
            break;
        }
        if (cancel.load()) {
            result.end = End::Stopped;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.end = End::TimedOut;
            break;
        }
    }
    GetExitCodeProcess(process.get(), &result.code);
    TerminateJobObject(job.get(), 1);
    reader.join();
    writer.join();

    result.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (output.starts_with("\xEF\xBB\xBF")) {
        output.erase(0, 3);
    }
    return result;
}

// The text a command's result is reported as: how it ended, then its output.
ToolResult Describe(const ProcessResult& run, int timeout, bool clixml) {
    if (!run.error.empty()) {
        return Error(run.error);
    }
    std::string header;
    switch (run.end) {
        case End::Exited:
            header = std::format("[exit code {}, {} ms]", static_cast<int>(run.code), run.ms);
            break;
        case End::TimedOut:
            header = std::format("[timed out after {} s, killed]", timeout);
            break;
        case End::Stopped:
            header = "[stopped by the user, killed]";
            break;
    }
    return ToolResult{header + "\n" + Shorten(clixml ? DecodeClixml(run.output) : run.output),
                      run.end != End::Exited || run.code != 0};
}

// Runs `script` on a server through the system ssh.exe: keys only (no prompts), a new host's key is
// remembered, stdin is `input`. The script travels as base64 in the remote command line, so no quoting
// can break it and a command that reads its stdin cannot eat the rest of the script.
ProcessResult RunSsh(const Target& target, const std::string& script, const std::string& input, int timeout,
                     const std::atomic<bool>& cancel) {
    const std::string remote =
        "B=$(command -v bash || echo sh); $B -c \"$(printf %s " + Base64Bytes(script) + " | base64 -d)\"";
    std::wstring line = L"ssh.exe -T -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 "
                        L"-o StrictHostKeyChecking=accept-new";
    if (!target.port.empty()) {
        line += L" -p " + ToWide(target.port);
    }
    line += L" " + ToWide(target.host) + L" \"";
    for (const char c : remote) {
        if (c == '"') {
            line += L'\\';
        }
        line += static_cast<wchar_t>(c);
    }
    line += L'"';
    if (line.size() > 30000) {
        ProcessResult too;
        too.error = "The command is too long for ssh's command line; put it in a file with write_file and run that";
        return too;
    }
    return RunProcess(std::move(line), MakeEnvironmentBlock({}), L"", timeout, input, cancel);
}

// What an edit_file call does to `text`; false with `error` when it cannot be done.
bool ApplyEdit(std::string& text, const Json& input, const std::string& where, size_t& count, std::string& error) {
    std::string from = StringArg(input, "old_string");
    std::string to = StringArg(input, "new_string");
    if (from.empty()) {
        error = "path and a non-empty old_string are required";
        return false;
    }
    count = CountOccurrences(text, from);
    // A CRLF file edited with LF text: the same edit in the file's own line endings.
    if (count == 0 && text.find("\r\n") != std::string::npos && from.find('\n') != std::string::npos) {
        from = WithCrLf(from);
        to = WithCrLf(to);
        count = CountOccurrences(text, from);
    }
    if (count == 0) {
        error = "old_string not found in " + where;
        return false;
    }
    if (count > 1 && !BoolArg(input, "replace_all")) {
        error = std::format("old_string occurs {} times; add context or set replace_all", count);
        return false;
    }
    text = ReplaceAll(std::move(text), from, to);
    return true;
}

// The command, the secrets and the timeout of a run_command call; false with `error` when it cannot run.
bool PrepareCommand(const SecretStore& secrets, const Json& input, std::string& command,
                    std::vector<std::pair<std::wstring, std::wstring>>& overrides, int& timeout, ToolResult& error) {
    const std::string raw = StringArg(input, "command");
    if (raw.empty()) {
        error = Error("command is required");
        return false;
    }
    std::vector<std::string> missing;
    command = secrets.Substitute(raw, missing);
    std::vector<std::string> names;
    if (const Json& list = JsonAt(input, "env_secrets"); list.is_array()) {
        for (const Json& item : list) {
            if (item.is_string()) {
                names.push_back(item.get<std::string>());
            }
        }
    }
    overrides = secrets.Environment(names, missing);
    if (!missing.empty()) {
        std::string text = "Unknown secret(s):";
        for (const std::string& name : missing) {
            text += " " + name;
        }
        error = Error(text + ". Nothing was run.");
        return false;
    }
    timeout = kDefaultTimeout;
    if (const Json& value = JsonAt(input, "timeout_seconds"); value.is_number_integer()) {
        timeout = std::clamp(value.get<int>(), 1, kMaxTimeout);
    }
    return true;
}

}  // namespace

std::string OwnToolName(const std::string& fullName) {
    const std::string prefix = std::string("mcp__") + kToolServer + "__";
    return fullName.starts_with(prefix) ? fullName.substr(prefix.size()) : std::string();
}

bool ToolNeedsApproval(const std::string& name, const Json& input) {
    if (!StringArg(input, "on").empty()) {  // a server is not ours to look around in unasked
        return true;
    }
    return name != "read_file" && name != "list_directory";
}

namespace {

ToolSummary SummarizeHere(const std::string& name, const Json& input) {
    const std::wstring path = ToWide(StringArg(input, "path"));
    if (name == "run_command") {
        std::wstring detail = ToWide(StringArg(input, "command"));
        if (const std::string dir = StringArg(input, "cwd"); !dir.empty()) {
            detail = L"[" + ToWide(dir) + L"]\n" + detail;
        }
        const Json& secrets = JsonAt(input, "env_secrets");
        if (secrets.is_array() && !secrets.empty()) {
            std::wstring names;
            for (const Json& item : secrets) {
                names += (names.empty() ? L"" : L", ") + ToWide(item.is_string() ? item.get<std::string>() : "?");
            }
            detail += L"\n\nСекреты: " + names;
        }
        return {L"Выполнить команду", std::move(detail)};
    }
    if (name == "write_file") {
        const std::string content = StringArg(input, "content");
        return {L"Записать файл", path + L"\n" + ToWide(Truncated(content, 800))};
    }
    if (name == "edit_file") {
        return {L"Изменить файл", path + L"\n− " + ToWide(Truncated(StringArg(input, "old_string"), 400)) + L"\n+ " +
                                      ToWide(Truncated(StringArg(input, "new_string"), 400))};
    }
    if (name == "read_file") {
        return {L"Прочитать файл", path};
    }
    if (name == "list_directory") {
        return {L"Показать папку", path.empty() ? L"." : path};
    }
    return {ToWide(name), ToWide(Truncated(input.dump(-1, ' ', false, Json::error_handler_t::replace), 800))};
}

}  // namespace

ToolSummary SummarizeTool(const std::string& name, const Json& input) {
    ToolSummary summary = SummarizeHere(name, input);
    if (const std::string on = StringArg(input, "on"); !on.empty()) {
        summary.detail = L"→ " + ToWide(on) + L"\n" + summary.detail;
    }
    return summary;
}

Tools::Tools(SecretStore& secrets, std::wstring cwd) : secrets_(secrets), cwd_(std::move(cwd)) {}

std::string Tools::SystemPrompt() {
    return "You are running inside AIControls, a native Windows chat client, on the user's computer. Claude Code's "
           "own local tools are off: use only the mcp__aic__* tools run_command, read_file, list_directory, "
           "write_file and edit_file. On this computer run_command is PowerShell 5.1 (non-interactive). Paths are "
           "paths on this computer; relative ones resolve against the chat's working directory. Every run_command, "
           "write_file and edit_file call, and anything aimed at a server, is shown to the user for approval, so "
           "keep commands focused and say briefly why you run them. "
           "Every tool accepts `on`: user@host[:port] makes it act on that server instead, through ssh with key "
           "login only (no passwords): run_command runs bash or sh there, the file tools read and write the "
           "server's files (remote paths are absolute or ~/...). This is the only chat: a task that spans this "
           "computer and any number of servers stays in one conversation. Each call is a fresh shell: cd, export "
           "and set -e do not carry over, so use cwd or chain with &&. "
           "Stored secrets are passed with env_secrets or written as {{secret:NAME}}; their values are cut out of "
           "every output, so you never see them. "
           "An HTML or SVG file you write or edit on this computer opens in the client's live preview beside the "
           "chat, and it reloads whenever the file or a CSS/JS file beside it changes: for an interface mockup, write "
           "one self-contained HTML file and refine it in place with edit_file. Paths you mention as C:\\... are "
           "clickable for the user. "
           "Prefer non-interactive commands (no pagers, no editors; -y only when the user asked for changes). "
           "Reply in the language the user writes in.";
}

Json Tools::Definitions() const {
    std::string secretList;
    for (const std::string& name : secrets_.Names()) {
        secretList += (secretList.empty() ? "" : ", ") + name;
    }
    const std::string known = secretList.empty() ? std::string(" No secrets are stored yet.") : " Stored secrets: " + secretList + ".";
    const Json str{{"type", "string"}};
    const Json on{{"type", "string"},
                  {"description",
                   "Do it on this server instead of this computer: user@host or user@host:port (key login through the "
                   "system ssh). Paths and commands are then the server's; its shell is bash or sh, not PowerShell."}};
    return Json::array({
        Json{{"name", "run_command"},
             {"description",
              "Run a PowerShell command on this computer (non-interactive; with `on`, a bash/sh command on that "
              "server) and return its combined output and exit "
              "code. Secrets: list names in env_secrets (they arrive as environment variables) or write "
              "{{secret:NAME}} in the command; their values are cut out of the output, so you never see them." + known},
             {"inputSchema",
              Schema({{"on", on},
                      {"command", {{"type", "string"}, {"description", "PowerShell command (bash/sh with `on`)"}}},
                      {"cwd", {{"type", "string"}, {"description", "Working directory (default: the chat's)"}}},
                      {"timeout_seconds",
                       {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxTimeout},
                        {"description", "Kill after this many seconds (default 120)"}}},
                      {"env_secrets",
                       {{"type", "array"}, {"items", str},
                        {"description", "Names of stored secrets to pass as environment variables"}}}},
                     Json::array({"command"}))}},
        Json{{"name", "read_file"},
             {"description", "Read a text file on this computer (up to 400 KB)."},
             {"inputSchema", Schema({{"on", on}, {"path", str}}, Json::array({"path"}))}},
        Json{{"name", "write_file"},
             {"description", "Create or overwrite a text file (UTF-8). Missing folders are created."},
             {"inputSchema",
              Schema({{"on", on}, {"path", str}, {"content", str}}, Json::array({"path", "content"}))}},
        Json{{"name", "edit_file"},
             {"description",
              "Replace an exact text fragment in a file. old_string must occur exactly once unless replace_all."},
             {"inputSchema",
              Schema({{"on", on}, {"path", str}, {"old_string", str}, {"new_string", str},
                      {"replace_all", {{"type", "boolean"}}}},
                     Json::array({"path", "old_string", "new_string"}))}},
        Json{{"name", "list_directory"},
             {"description", "List a folder on this computer (default: the chat's working directory)."},
             {"inputSchema", Schema({{"on", on}, {"path", str}}, Json::array())}},
    });
}

ToolResult Tools::Execute(const std::string& name, const Json& input, const std::atomic<bool>& cancel) const {
    try {
        ToolResult result;
        if (const std::string on = StringArg(input, "on"); !on.empty()) {
            result = ExecuteOn(on, name, input, cancel);
        } else if (name == "run_command") {
            result = RunCommand(input, cancel);
        } else if (name == "read_file") {
            result = ReadFile(input);
        } else if (name == "write_file") {
            result = WriteFile(input);
        } else if (name == "edit_file") {
            result = EditFile(input);
        } else if (name == "list_directory") {
            result = ListDirectory(input);
        } else {
            return Error("Unknown tool: " + name);
        }
        result.text = secrets_.Redact(result.text);
        return result;
    } catch (const std::exception& e) {
        return Error(std::string("Tool failed: ") + e.what());
    }
}

std::wstring Tools::Resolve(const std::string& path) const {
    fs::path p(ToWide(path));
    if (p.empty()) {
        return cwd_;
    }
    return p.is_relative() ? (fs::path(cwd_) / p).wstring() : p.wstring();
}

ToolResult Tools::RunCommand(const Json& input, const std::atomic<bool>& cancel) const {
    std::string command;
    std::vector<std::pair<std::wstring, std::wstring>> overrides;
    int timeout = 0;
    ToolResult error;
    if (!PrepareCommand(secrets_, input, command, overrides, timeout, error)) {
        return error;
    }
    const std::wstring dir = Resolve(StringArg(input, "cwd"));

    // The script: UTF-8 in and out, and the exit code of the last command (or 1 if it failed without one).
    // Errors go to the output as plain text (ErrorRecord.ToString: the message, no script excerpts); with
    // -EncodedCommand the host would wrap them in CLIXML on stderr. What cannot be caught here, a syntax
    // error, still arrives as CLIXML and is decoded below.
    const std::wstring script =
        L"$ProgressPreference='SilentlyContinue';[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
        L"$OutputEncoding=[Console]::OutputEncoding;$global:AicOk=$true;$global:AicCode=0;"
        L"try{& {\n" +
        ToWide(command) +
        L"\n$global:AicOk=$?;$global:AicCode=$LASTEXITCODE} 2>&1 | ForEach-Object{"
        L"if($_ -is [Management.Automation.ErrorRecord]){$_.ToString()}else{$_}}}"
        L"catch{[Console]::Out.WriteLine($_.ToString());$global:AicOk=$false}\n"
        L"exit $(if($global:AicCode){$global:AicCode}elseif($global:AicOk){0}else{1})";
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
                       L"-OutputFormat Text -EncodedCommand " +
                       ToWide(Base64(script));
    return Describe(RunProcess(std::move(cmd), MakeEnvironmentBlock(overrides), dir, timeout, {}, cancel), timeout,
                    true);
}

// The same five tools on a server (`on`): the shell there is bash or sh, files go through cat and ls.
ToolResult Tools::ExecuteOn(const std::string& on, const std::string& name, const Json& input,
                            const std::atomic<bool>& cancel) const {
    const std::optional<Target> target = ParseTarget(on);
    if (!target) {
        return Error("Bad `on`: expected user@host or user@host:port, got: " + on);
    }
    const std::string path = StringArg(input, "path");
    // A file operation ends here: the output is the result when it worked, the reason when it did not.
    const auto report = [&](const ProcessResult& run, std::string success) {
        if (!run.error.empty()) {
            return Error(run.error);
        }
        if (run.end != End::Exited || run.code != 0) {
            return Describe(run, 30, false);
        }
        return ToolResult{std::move(success), false};
    };
    const auto write = [&](const std::string& file, const std::string& content) {
        const std::string script = "p=" + ShPath(file) + "\nmkdir -p -- \"$(dirname -- \"$p\")\" && cat > \"$p\"\n";
        return report(RunSsh(*target, script, content, 60, cancel),
                      std::format("Wrote {} bytes to {}:{}", content.size(), on, file));
    };

    if (name == "run_command") {
        std::string command;
        std::vector<std::pair<std::wstring, std::wstring>> overrides;
        int timeout = 0;
        ToolResult error;
        if (!PrepareCommand(secrets_, input, command, overrides, timeout, error)) {
            return error;
        }
        std::string script;
        for (const auto& [variable, value] : overrides) {
            const std::string key = ToUtf8(variable);
            if (key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") == std::string::npos) {
                script += "export " + key + "=" + ShQuote(ToUtf8(value)) + "\n";
            }
        }
        if (const std::string dir = StringArg(input, "cwd"); !dir.empty()) {
            script += "cd " + ShPath(dir) + " || exit 1\n";
        }
        script += command + "\n";
        return Describe(RunSsh(*target, script, {}, timeout, cancel), timeout, false);
    }
    if (name == "read_file") {
        if (path.empty()) {
            return Error("path is required");
        }
        const ProcessResult run = RunSsh(*target,
                                         "f=" + ShPath(path) +
                                             "\nif [ -d \"$f\" ]; then echo \"Is a folder; use list_directory: $f\"; exit 2; fi\n"
                                             "head -c " + std::to_string(kReadLimit + 1) + " -- \"$f\"\n",
                                         {}, 60, cancel);
        std::string text = run.output;
        if (text.size() > kReadLimit) {
            text.resize(kReadLimit);
            text += std::format("\n... [the file is longer; the first {} bytes shown]", kReadLimit);
        }
        return report(run, std::move(text));
    }
    if (name == "write_file") {
        if (path.empty() || !JsonAt(input, "content").is_string()) {
            return Error("path and content are required");
        }
        return write(path, StringArg(input, "content"));
    }
    if (name == "edit_file") {
        if (path.empty()) {
            return Error("path and a non-empty old_string are required");
        }
        const ProcessResult read = RunSsh(*target, "cat -- " + ShPath(path) + "\n", {}, 60, cancel);
        if (!read.error.empty() || read.end != End::Exited || read.code != 0) {
            return report(read, {});
        }
        std::string text = read.output;
        size_t count = 0;
        std::string reason;
        if (!ApplyEdit(text, input, on + ":" + path, count, reason)) {
            return Error(reason);
        }
        const ToolResult written = write(path, text);
        return written.isError ? written
                               : ToolResult{std::format("Replaced {} occurrence(s) in {}:{}", count, on, path), false};
    }
    if (name == "list_directory") {
        const std::string dir = ShPath(path.empty() ? "." : path);
        const std::string script = "d=" + dir +
                                   "\nout=$(ls -lA -- \"$d\" 2>&1) || { printf '%s\\n' \"$out\"; exit 1; }\n"
                                   "(cd \"$d\" 2>/dev/null && pwd) || echo \"$d\"\n"
                                   "printf '%s\\n' \"$out\" | head -n " + std::to_string(kListLimit + 1) + "\n";
        const ProcessResult run = RunSsh(*target, script, {}, 60, cancel);
        return report(run, run.output);
    }
    return Error("Unknown tool: " + name);
}

ToolResult Tools::ReadFile(const Json& input) const {
    const std::string path = StringArg(input, "path");
    if (path.empty()) {
        return Error("path is required");
    }
    const fs::path file = Resolve(path);
    std::error_code ec;
    if (fs::is_directory(file, ec)) {
        return Error("Is a folder; use list_directory: " + ToUtf8(file.wstring()));
    }
    std::string bytes;
    if (!ReadWhole(file, bytes)) {
        return Error("Cannot read: " + ToUtf8(file.wstring()));
    }
    if (bytes.starts_with("\xFF\xFE")) {  // UTF-16 LE with BOM (PowerShell's favourite)
        const std::wstring wide(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (bytes.size() - 2) / 2);
        bytes = ToUtf8(wide);
    } else if (bytes.starts_with("\xEF\xBB\xBF")) {
        bytes.erase(0, 3);
    }
    if (bytes.size() > kReadLimit) {
        const size_t total = bytes.size();
        bytes.resize(kReadLimit);
        bytes += std::format("\n… [file is {} bytes; the first {} shown]", total, kReadLimit);
    }
    return ToolResult{std::move(bytes), false};
}

ToolResult Tools::WriteFile(const Json& input) const {
    const std::string path = StringArg(input, "path");
    if (path.empty() || !JsonAt(input, "content").is_string()) {
        return Error("path and content are required");
    }
    const fs::path file = Resolve(path);
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
    }
    const std::string content = StringArg(input, "content");
    if (!WriteWhole(file, content)) {
        return Error("Cannot write: " + ToUtf8(file.wstring()));
    }
    return ToolResult{std::format("Wrote {} bytes to {}", content.size(), ToUtf8(file.wstring())), false};
}

ToolResult Tools::EditFile(const Json& input) const {
    const std::string path = StringArg(input, "path");
    if (path.empty()) {
        return Error("path and a non-empty old_string are required");
    }
    const fs::path file = Resolve(path);
    std::string text;
    if (!ReadWhole(file, text)) {
        return Error("Cannot read: " + ToUtf8(file.wstring()));
    }
    size_t count = 0;
    std::string reason;
    if (!ApplyEdit(text, input, ToUtf8(file.wstring()), count, reason)) {
        return Error(reason);
    }
    if (!WriteWhole(file, text)) {
        return Error("Cannot write: " + ToUtf8(file.wstring()));
    }
    return ToolResult{std::format("Replaced {} occurrence(s) in {}", count, ToUtf8(file.wstring())), false};
}

ToolResult Tools::ListDirectory(const Json& input) const {
    const fs::path dir = Resolve(StringArg(input, "path"));
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> dirs, files;
    for (fs::directory_iterator it(dir, ec), last; !ec && it != last; it.increment(ec)) {
        std::error_code entryError;
        const std::string name = ToUtf8(it->path().filename().wstring());
        if (it->is_directory(entryError)) {
            dirs.emplace_back(name + "/", std::string());
        } else {
            const auto size = it->file_size(entryError);
            files.emplace_back(name, entryError ? std::string() : std::format("  ({} bytes)", size));
        }
    }
    if (ec && dirs.empty() && files.empty()) {
        return Error("Cannot list: " + ToUtf8(dir.wstring()));
    }
    std::ranges::sort(dirs);
    std::ranges::sort(files);
    std::string text = ToUtf8(dir.wstring()) + "\n";
    size_t shown = 0;
    for (const auto* group : {&dirs, &files}) {
        for (const auto& [name, extra] : *group) {
            if (shown++ == kListLimit) {
                text += std::format("… and {} more\n", dirs.size() + files.size() - kListLimit);
                return ToolResult{std::move(text), false};
            }
            text += name + extra + "\n";
        }
    }
    return ToolResult{std::move(text), false};
}

ToolHost::ToolHost(Tools& tools, Reply reply) : tools_(tools), reply_(std::move(reply)) {}

ToolHost::~ToolHost() {
    CancelAll();
    std::lock_guard lock(mutex_);
    for (Job& job : jobs_) {
        if (job.thread.joinable()) {
            job.thread.join();
        }
    }
}

void ToolHost::CancelAll() {
    std::lock_guard lock(mutex_);
    for (Job& job : jobs_) {
        job.cancel = true;
    }
}

void ToolHost::Reap() {
    std::lock_guard lock(mutex_);
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->done) {
            it->thread.join();
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

void ToolHost::HandleMcp(const std::string& requestId, const Json& request) {
    Reap();
    const Json& message = JsonAt(request, "message");
    const std::string method = JsonStr(message, "method");
    const Json& params = JsonAt(message, "params");
    Json id = nullptr;
    if (const auto it = message.find("id"); message.is_object() && it != message.end()) {
        id = *it;
    }
    const auto answer = [&](Json result) {
        reply_(requestId, Json{{"mcp_response", {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}}});
    };

    if (method == "initialize") {
        answer(Json{{"protocolVersion", JsonStr(params, "protocolVersion")},
                    {"capabilities", {{"tools", Json::object()}}},
                    {"serverInfo", {{"name", kToolServer}, {"version", "0.1.0"}}}});
    } else if (method == "tools/list") {
        answer(Json{{"tools", tools_.Definitions()}});
    } else if (method == "tools/call") {
        std::lock_guard lock(mutex_);
        Job& job = jobs_.emplace_back();
        const std::string name = JsonStr(params, "name");
        Json arguments = JsonAt(params, "arguments");
        job.thread = std::thread([this, &job, requestId, id, name, arguments = std::move(arguments)] {
            const ToolResult result = tools_.Execute(name, arguments, job.cancel);
            reply_(requestId,
                   Json{{"mcp_response",
                         {{"jsonrpc", "2.0"},
                          {"id", id},
                          {"result",
                           {{"content", Json::array({Json{{"type", "text"}, {"text", result.text}}})},
                            {"isError", result.isError}}}}}});
            job.done = true;
        });
    } else {
        answer(Json::object());  // notifications/initialized, ping …
    }
}

}  // namespace aic
