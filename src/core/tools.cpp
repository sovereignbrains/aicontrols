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
#include <stdexcept>
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

std::string Base64(const std::wstring& text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t size = text.size() * sizeof(wchar_t);
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const unsigned chunk = (static_cast<unsigned>(bytes[i]) << 16) |
                               (i + 1 < size ? static_cast<unsigned>(bytes[i + 1]) << 8 : 0u) |
                               (i + 2 < size ? static_cast<unsigned>(bytes[i + 2]) : 0u);
        out += alphabet[(chunk >> 18) & 63];
        out += alphabet[(chunk >> 12) & 63];
        out += i + 1 < size ? alphabet[(chunk >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[chunk & 63] : '=';
    }
    return out;
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

}  // namespace

std::string OwnToolName(const std::string& fullName) {
    const std::string prefix = std::string("mcp__") + kToolServer + "__";
    return fullName.starts_with(prefix) ? fullName.substr(prefix.size()) : std::string();
}

bool ToolNeedsApproval(const std::string& name) {
    return name != "read_file" && name != "list_directory";
}

ToolSummary SummarizeTool(const std::string& name, const Json& input) {
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

Tools::Tools(SecretStore& secrets, std::wstring cwd) : secrets_(secrets), cwd_(std::move(cwd)) {}

Json Tools::Definitions() {
    const Json str{{"type", "string"}};
    return Json::array({
        Json{{"name", "run_command"},
             {"description",
              "Run a PowerShell command on this computer (non-interactive) and return its combined output and exit "
              "code. Secrets: list names in env_secrets (they arrive as environment variables) or write "
              "{{secret:NAME}} in the command; their values are cut out of the output, so you never see them."},
             {"inputSchema",
              Schema({{"command", {{"type", "string"}, {"description", "PowerShell command"}}},
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
             {"inputSchema", Schema({{"path", str}}, Json::array({"path"}))}},
        Json{{"name", "write_file"},
             {"description", "Create or overwrite a text file (UTF-8). Missing folders are created."},
             {"inputSchema", Schema({{"path", str}, {"content", str}}, Json::array({"path", "content"}))}},
        Json{{"name", "edit_file"},
             {"description",
              "Replace an exact text fragment in a file. old_string must occur exactly once unless replace_all."},
             {"inputSchema",
              Schema({{"path", str}, {"old_string", str}, {"new_string", str}, {"replace_all", {{"type", "boolean"}}}},
                     Json::array({"path", "old_string", "new_string"}))}},
        Json{{"name", "list_directory"},
             {"description", "List a folder on this computer (default: the chat's working directory)."},
             {"inputSchema", Schema({{"path", str}}, Json::array())}},
    });
}

ToolResult Tools::Execute(const std::string& name, const Json& input, const std::atomic<bool>& cancel) const {
    try {
        ToolResult result;
        if (name == "run_command") {
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
    const std::string raw = StringArg(input, "command");
    if (raw.empty()) {
        return Error("command is required");
    }
    std::vector<std::string> missing;
    const std::string command = secrets_.Substitute(raw, missing);
    std::vector<std::string> names;
    if (const Json& list = JsonAt(input, "env_secrets"); list.is_array()) {
        for (const Json& item : list) {
            if (item.is_string()) {
                names.push_back(item.get<std::string>());
            }
        }
    }
    const auto overrides = secrets_.Environment(names, missing);
    if (!missing.empty()) {
        std::string text = "Unknown secret(s):";
        for (const std::string& name : missing) {
            text += " " + name;
        }
        return Error(text + ". Nothing was run.");
    }
    int timeout = kDefaultTimeout;
    if (const Json& value = JsonAt(input, "timeout_seconds"); value.is_number_integer()) {
        timeout = std::clamp(value.get<int>(), 1, kMaxTimeout);
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
    std::vector<wchar_t> environment = MakeEnvironmentBlock(overrides);

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    wil::unique_handle outRead, outWrite;
    if (!CreatePipe(outRead.put(), outWrite.put(), &inheritable, 1 << 16)) {
        return Error("CreatePipe failed");
    }
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    // stdin is NUL: a command that waits for input ends instead of hanging.
    wil::unique_handle nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!nul) {
        return Error("Cannot open NUL");
    }

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<std::byte> attributesBuffer(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        return Error("InitializeProcThreadAttributeList failed");
    }
    auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
    std::array<HANDLE, 2> inherited{nul.get(), outWrite.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(inherited), nullptr, nullptr)) {
        return Error("UpdateProcThreadAttribute failed");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul.get();
    startup.StartupInfo.hStdOutput = outWrite.get();
    startup.StartupInfo.hStdError = outWrite.get();
    startup.lpAttributeList = attributes;

    wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        return Error("CreateJobObject failed");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    const auto started = std::chrono::steady_clock::now();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), dir.empty() ? nullptr : dir.c_str(), &startup.StartupInfo, &info)) {
        return Error(std::format("Cannot start powershell.exe (error {}); cwd: {}", GetLastError(), ToUtf8(dir)));
    }
    wil::unique_handle process(info.hProcess);
    const wil::unique_handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        return Error("AssignProcessToJobObject failed");
    }
    ResumeThread(thread.get());
    outWrite.reset();  // ours; without closing it the reader never sees the end

    std::string output;
    std::thread reader([&] {
        std::array<char, 8192> chunk{};
        DWORD got = 0;
        while (::ReadFile(outRead.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
            if (output.size() < kOutputHardCap) {
                output.append(chunk.data(), got);
            }
        }
    });

    enum class End { Exited, TimedOut, Stopped } end = End::Exited;
    const auto deadline = started + std::chrono::seconds(timeout);
    for (;;) {
        if (WaitForSingleObject(process.get(), 100) == WAIT_OBJECT_0) {
            break;
        }
        if (cancel.load()) {
            end = End::Stopped;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            end = End::TimedOut;
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(process.get(), &code);
    // Whatever the command left running dies with it, and the pipe closes.
    TerminateJobObject(job.get(), 1);
    reader.join();

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::string header;
    switch (end) {
        case End::Exited:
            header = std::format("[exit code {}, {} ms]", static_cast<int>(code), elapsed.count());
            break;
        case End::TimedOut:
            header = std::format("[timed out after {} s, killed]", timeout);
            break;
        case End::Stopped:
            header = "[stopped by the user, killed]";
            break;
    }
    if (output.starts_with("\xEF\xBB\xBF")) {
        output.erase(0, 3);
    }
    ToolResult result{header + "\n" + Shorten(DecodeClixml(output)), end != End::Exited || code != 0};
    return result;
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
    std::string from = StringArg(input, "old_string");
    std::string to = StringArg(input, "new_string");
    if (path.empty() || from.empty()) {
        return Error("path and a non-empty old_string are required");
    }
    const fs::path file = Resolve(path);
    std::string text;
    if (!ReadWhole(file, text)) {
        return Error("Cannot read: " + ToUtf8(file.wstring()));
    }
    size_t count = CountOccurrences(text, from);
    // A CRLF file edited with LF text: the same edit in the file's own line endings.
    if (count == 0 && text.find("\r\n") != std::string::npos && from.find('\n') != std::string::npos) {
        from = WithCrLf(from);
        to = WithCrLf(to);
        count = CountOccurrences(text, from);
    }
    if (count == 0) {
        return Error("old_string not found in " + ToUtf8(file.wstring()));
    }
    if (count > 1 && !BoolArg(input, "replace_all")) {
        return Error(std::format("old_string occurs {} times; add context or set replace_all", count));
    }
    text = ReplaceAll(std::move(text), from, to);
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
        answer(Json{{"tools", Tools::Definitions()}});
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
