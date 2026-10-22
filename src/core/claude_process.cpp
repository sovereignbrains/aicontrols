#include "core/claude_process.h"

#include <winsock2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <map>
#include <stdexcept>
#include <utility>

namespace aic {
namespace {

constexpr const char* kInitRequestId = "aic_init";
constexpr size_t kStderrTail = 8192;

[[noreturn]] void ThrowLastError(const char* what) {
    const DWORD error = GetLastError();
    throw std::runtime_error(std::string(what) + " failed, error " + std::to_string(error));
}

// One argument for CommandLineToArgvW / the CRT's parser.
std::wstring QuoteArg(std::wstring_view arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out += c;
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}

struct NoCaseLess {
    bool operator()(const std::wstring& a, const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                    static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    }
};

// Our environment with the overrides applied, as a sorted CREATE_UNICODE_ENVIRONMENT block.
std::vector<wchar_t> BuildEnvironment(const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    std::map<std::wstring, std::wstring, NoCaseLess> vars;
    LPWCH env = GetEnvironmentStringsW();
    if (env != nullptr) {
        for (const wchar_t* entry = env; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
            const std::wstring_view text(entry);
            const size_t eq = text.find(L'=', 1);  // "=C:=C:\..." entries start with '='
            if (eq != std::wstring_view::npos) {
                vars[std::wstring(text.substr(0, eq))] = std::wstring(text.substr(eq + 1));
            }
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [name, value] : overrides) {
        vars[name] = value;
    }
    std::vector<wchar_t> block;
    for (const auto& [name, value] : vars) {
        block.insert(block.end(), name.begin(), name.end());
        block.push_back(L'=');
        block.insert(block.end(), value.begin(), value.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

bool PortOpen(uint16_t port) {
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // A refused loopback connect takes Windows ~2 s of SYN retries; a listener answers at once.
    connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(s, &writable);
    timeval timeout{0, 300'000};
    const bool open = select(0, nullptr, &writable, nullptr, &timeout) > 0 && FD_ISSET(s, &writable);
    closesocket(s);
    return open;
}

}  // namespace

ClaudeProcess::~ClaudeProcess() {
    Stop();
}

void ClaudeProcess::Start(const ClaudeOptions& options, MessageHandler onMessage, ExitHandler onExit) {
    Stop();
    stopping_ = false;
    onMessage_ = std::move(onMessage);
    onExit_ = std::move(onExit);
    {
        std::lock_guard lock(stderrMutex_);
        stderrTail_.clear();
    }

    const std::wstring exe = options.exe.empty() ? FindClaudeExe() : options.exe;
    if (exe.empty()) {
        throw std::runtime_error("claude.exe not found");
    }
    std::wstring cmd = QuoteArg(exe) +
                       L" --output-format stream-json --verbose --input-format stream-json"
                       L" --permission-prompt-tool stdio --tools \"\""
                       L" --mcp-config {\\\"mcpServers\\\":{\\\"aic\\\":{\\\"type\\\":\\\"sdk\\\",\\\"name\\\":\\\"aic\\\"}}}";
    if (options.includePartial) {
        cmd += L" --include-partial-messages";
    }
    if (!options.appendSystemPrompt.empty()) {
        cmd += L" --append-system-prompt " + QuoteArg(ToWide(options.appendSystemPrompt));
    }
    if (!options.settings.empty()) {
        cmd += L" --settings " + QuoteArg(ToWide(options.settings));
    }
    if (!options.model.empty()) {
        cmd += L" --model " + QuoteArg(ToWide(options.model));
    }
    if (!options.effort.empty()) {
        cmd += L" --effort " + QuoteArg(ToWide(options.effort));
    }
    if (!options.resume.empty()) {
        cmd += L" --resume " + QuoteArg(ToWide(options.resume));
    }

    // session_state_changed (running / requires_action / idle): the only reliable "is it still working",
    // since a message sent mid-turn is folded into the running turn and gets no result of its own.
    std::vector<std::pair<std::wstring, std::wstring>> overrides = {{L"CLAUDE_CODE_SDK_READS_SESSION_STATE", L"1"}};
    if (!options.proxy.empty()) {
        overrides.insert(overrides.end(), {{L"HTTPS_PROXY", options.proxy},
                                           {L"HTTP_PROXY", options.proxy},
                                           {L"NO_PROXY", L"127.0.0.1,localhost,::1"}});
    }
    std::vector<wchar_t> environment = BuildEnvironment(overrides);

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    wil::unique_handle inRead, inWrite, outRead, outWrite, errRead, errWrite;
    if (!CreatePipe(inRead.put(), inWrite.put(), &inheritable, 1 << 20) ||
        !CreatePipe(outRead.put(), outWrite.put(), &inheritable, 1 << 16) ||
        !CreatePipe(errRead.put(), errWrite.put(), &inheritable, 0)) {
        ThrowLastError("CreatePipe");
    }
    SetHandleInformation(inWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0);

    // Only the three pipe ends are inherited, not whatever else this process has open.
    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<std::byte> attributesBuffer(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        ThrowLastError("InitializeProcThreadAttributeList");
    }
    auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
    std::array<HANDLE, 3> inherited{inRead.get(), outWrite.get(), errWrite.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                   sizeof(inherited), nullptr, nullptr)) {
        ThrowLastError("UpdateProcThreadAttribute");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inRead.get();
    startup.StartupInfo.hStdOutput = outWrite.get();
    startup.StartupInfo.hStdError = errWrite.get();
    startup.lpAttributeList = attributes;

    // The CLI and everything it spawns die with the job, so nothing outlives a chat.
    wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        ThrowLastError("CreateJobObject");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        ThrowLastError("SetInformationJobObject");
    }

    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT,
                        environment.data(), options.cwd.empty() ? nullptr : options.cwd.c_str(),
                        &startup.StartupInfo, &info)) {
        ThrowLastError("CreateProcess");
    }
    wil::unique_handle process(info.hProcess);
    const wil::unique_handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        ThrowLastError("AssignProcessToJobObject");
    }
    ResumeThread(thread.get());

    // Our copies of the child's ends: without closing them the reads never see EOF.
    inRead.reset();
    outWrite.reset();
    errWrite.reset();

    process_ = std::move(process);
    job_ = std::move(job);
    stdoutRead_ = std::move(outRead);
    stderrRead_ = std::move(errRead);
    {
        std::lock_guard lock(writeMutex_);
        stdinWrite_ = std::move(inWrite);
        initialized_ = false;
        pending_.clear();
        const Json initialize{{"type", "control_request"},
                              {"request_id", kInitRequestId},
                              {"request", {{"subtype", "initialize"}, {"hooks", nullptr}}}};
        WriteLocked(initialize.dump() + "\n");
    }
    errorReader_ = std::thread(&ClaudeProcess::ReadErrors, this);
    reader_ = std::thread(&ClaudeProcess::ReadLoop, this);
}

void ClaudeProcess::Stop() {
    stopping_ = true;
    {
        std::lock_guard lock(writeMutex_);
        stdinWrite_.reset();  // the CLI's cue to finish
        initialized_ = false;
        pending_.clear();
    }
    if (process_) {
        WaitForSingleObject(process_.get(), 1500);
        // Also takes down anything it left behind still holding our pipes.
        TerminateJobObject(job_.get(), 1);
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    job_.reset();
    process_.reset();
    stdoutRead_.reset();
    stderrRead_.reset();
}

void ClaudeProcess::SendUserText(std::string_view text) {
    SendUserContent(Json(std::string(text)));
}

void ClaudeProcess::SendUserContent(Json content) {
    Send(Json{{"type", "user"},
              {"session_id", "default"},
              {"parent_tool_use_id", nullptr},
              {"message", {{"role", "user"}, {"content", std::move(content)}}}},
         true);
}

void ClaudeProcess::Interrupt() {
    Send(Json{{"type", "control_request"},
              {"request_id", "aic_" + std::to_string(++requestCounter_)},
              {"request", {{"subtype", "interrupt"}}}},
         true);
}

void ClaudeProcess::Respond(const std::string& requestId, Json response) {
    Send(Json{{"type", "control_response"},
              {"response", {{"subtype", "success"}, {"request_id", requestId}, {"response", std::move(response)}}}},
         false);
}

void ClaudeProcess::RespondError(const std::string& requestId, std::string_view error) {
    Send(Json{{"type", "control_response"},
              {"response", {{"subtype", "error"}, {"request_id", requestId}, {"error", std::string(error)}}}},
         false);
}

void ClaudeProcess::Send(const Json& message, bool afterInit) {
    std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::lock_guard lock(writeMutex_);
    if (afterInit && !initialized_) {
        pending_.push_back(std::move(line));
        return;
    }
    WriteLocked(line);
}

void ClaudeProcess::WriteLocked(const std::string& line) {
    if (!stdinWrite_) {
        return;
    }
    const char* data = line.data();
    size_t left = line.size();
    while (left > 0) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(left, 1 << 20));
        if (!WriteFile(stdinWrite_.get(), data, chunk, &written, nullptr)) {
            return;  // the CLI is gone; the reader reports the exit
        }
        data += written;
        left -= written;
    }
}

void ClaudeProcess::ReadLoop() {
    std::string buffer;
    std::vector<char> chunk(16384);
    DWORD got = 0;
    while (ReadFile(stdoutRead_.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
        buffer.append(chunk.data(), got);
        size_t start = 0;
        for (size_t newline = buffer.find('\n'); newline != std::string::npos;
             newline = buffer.find('\n', start)) {
            std::string_view line(buffer.data() + start, newline - start);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            if (!line.empty()) {
                HandleLine(line);
            }
            start = newline + 1;
        }
        buffer.erase(0, start);
    }

    DWORD code = STILL_ACTIVE;
    if (WaitForSingleObject(process_.get(), 3000) == WAIT_OBJECT_0) {
        GetExitCodeProcess(process_.get(), &code);
    } else {
        TerminateJobObject(job_.get(), 1);  // stdout closed but the CLI hangs on: end it
    }
    if (errorReader_.joinable()) {
        errorReader_.join();
    }
    std::string tail;
    {
        std::lock_guard lock(stderrMutex_);
        tail = stderrTail_;
    }
    if (!stopping_ && onExit_) {
        onExit_(code, std::move(tail));
    }
}

void ClaudeProcess::ReadErrors() {
    std::array<char, 4096> chunk{};
    DWORD got = 0;
    while (ReadFile(stderrRead_.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
        std::lock_guard lock(stderrMutex_);
        stderrTail_.append(chunk.data(), got);
        if (stderrTail_.size() > kStderrTail) {
            stderrTail_.erase(0, stderrTail_.size() - kStderrTail);
        }
    }
}

void ClaudeProcess::HandleLine(std::string_view line) {
    Json message = Json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        message = Json{{"type", "aic_raw"}, {"text", std::string(line)}};
    }
    if (JsonStr(message, "type") == "control_response" &&
        JsonStr(JsonAt(message, "response"), "request_id") == kInitRequestId) {
        std::lock_guard lock(writeMutex_);
        initialized_ = true;
        for (const std::string& held : pending_) {
            WriteLocked(held);
        }
        pending_.clear();
    }
    if (!stopping_ && onMessage_) {
        onMessage_(std::move(message));
    }
}

std::vector<wchar_t> MakeEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    return BuildEnvironment(overrides);
}

std::wstring FindClaudeExe() {
    std::array<wchar_t, 2048> buffer{};
    const auto size = static_cast<DWORD>(buffer.size());
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer.data(), size);
    if (length > 0 && length < size) {
        std::wstring candidate = std::wstring(buffer.data(), length) + L"\\.local\\bin\\claude.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    length = SearchPathW(nullptr, L"claude.exe", nullptr, size, buffer.data(), nullptr);
    if (length > 0 && length < size) {
        return std::wstring(buffer.data(), length);
    }
    return {};
}

std::wstring PickProxy() {
    static const bool winsock = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!winsock) {
        return {};
    }
    for (const uint16_t port : {uint16_t{2092}, uint16_t{2080}}) {
        if (PortOpen(port)) {
            return L"http://127.0.0.1:" + std::to_wstring(port);
        }
    }
    return {};
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr,
                        nullptr);
    return out;
}

std::wstring ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string JsonStr(const Json& object, const char* key) {
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end() && it->is_string()) {
            return it->get<std::string>();
        }
    }
    return {};
}

const Json& JsonAt(const Json& object, const char* key) {
    static const Json empty = Json::object();
    if (object.is_object()) {
        const auto it = object.find(key);
        if (it != object.end()) {
            return *it;
        }
    }
    return empty;
}

}  // namespace aic
