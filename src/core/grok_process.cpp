#include "core/grok_process.h"

#include "core/tools.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

namespace aic {
namespace {

constexpr size_t kStderrTail = 8192;
constexpr auto kMcpWait = std::chrono::seconds(15);  // how long a session waits for Grok to connect to our tools
constexpr int64_t kTicksPerDollar = 10'000'000'000;  // costUsdTicks

namespace fs = std::filesystem;

std::string RandomToken() {
    std::random_device device;
    std::string token;
    for (int i = 0; i < 4; ++i) {
        char part[9];
        snprintf(part, sizeof(part), "%08x", device());
        token += part;
    }
    return token;
}

// What Grok may use of its own: the two tools that reach MCP servers (ours). Everything else it can do, it does
// through the five tools, where the user is asked and the secrets are kept.
constexpr const char* kProfile =
    "---\n"
    "name: aic\n"
    "description: AIControls chat\n"
    "tools: search_tool, use_tool\n"
    "---\n";

std::wstring WriteProfile() {
    std::array<wchar_t, 2048> buffer{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    fs::path dir = length > 0 && length < buffer.size() ? fs::path(std::wstring(buffer.data(), length)) : fs::temp_directory_path();
    dir /= L"AIControls";
    std::error_code error;
    fs::create_directories(dir, error);
    const fs::path file = dir / L"grok-agent.md";
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << kProfile;
    return file.wstring();
}

Json RpcError(const Json& id, int code, const char* message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// The text of a finished tool call, whatever shape Grok wraps it in.
std::string OutputText(const Json& rawOutput) {
    const Json& output = JsonAt(rawOutput, "output");
    for (const char* key : {"OkayOutput", "ErrorOutput", "Output"}) {
        if (output.is_object() && output.contains(key)) {
            const Json& inner = output[key];
            return inner.is_string() ? inner.get<std::string>()
                                     : inner.dump(-1, ' ', false, Json::error_handler_t::replace);
        }
    }
    if (output.is_string()) {
        return output.get<std::string>();
    }
    if (rawOutput.is_string()) {
        return rawOutput.get<std::string>();
    }
    return rawOutput.is_null() ? std::string() : rawOutput.dump(-1, ' ', false, Json::error_handler_t::replace);
}

long long Number(const Json& object, const char* key) {
    const Json& value = JsonAt(object, key);
    return value.is_number() ? value.get<long long>() : 0;
}

}  // namespace

GrokProcess::~GrokProcess() {
    Stop();
}

void GrokProcess::Start(const ClaudeOptions& options, MessageHandler onMessage, ExitHandler onExit) {
    Stop();
    stopping_ = false;
    options_ = options;
    onMessage_ = std::move(onMessage);
    onExit_ = std::move(onExit);
    {
        std::lock_guard lock(stderrMutex_);
        stderrTail_.clear();
    }
    {
        std::lock_guard lock(stateMutex_);
        nextId_ = 0;
        calls_.clear();
        sessionId_.clear();
        sessionOpen_ = false;
        mcpReady_ = false;
        turnActive_ = false;
        queue_.clear();
        pendingModel_.clear();
        textOpen_ = false;
        permissions_.clear();
        refusals_.clear();
        mcp_.clear();
    }

    const std::wstring exe = options.exe.empty() ? FindGrokExe() : options.exe;
    if (exe.empty()) {
        throw std::runtime_error("grok.exe not found");
    }
    token_ = RandomToken();
    serverPort_ = server_.Start(token_, [this](const std::string& body) { return HandleMcpRequest(body); });
    profilePath_ = WriteProfile();

    std::wstring cmd = QuoteArg(exe) + L" agent --no-leader --agent-profile " + QuoteArg(profilePath_);
    if (!options.model.empty()) {
        cmd += L" --model " + QuoteArg(ToWide(options.model));
    }
    if (!options.effort.empty()) {
        cmd += L" --effort " + QuoteArg(ToWide(options.effort));
    }
    cmd += L" stdio";

    std::vector<std::pair<std::wstring, std::wstring>> overrides = {{L"NO_COLOR", L"1"}};
    if (!options.proxy.empty()) {
        overrides.insert(overrides.end(), {{L"HTTPS_PROXY", options.proxy},
                                           {L"HTTP_PROXY", options.proxy},
                                           {L"NO_PROXY", L"127.0.0.1,localhost,::1"}});
    }
    SpawnedProcess spawned;
    try {
        spawned = SpawnPiped(exe, std::move(cmd), overrides, options.cwd);
    } catch (...) {
        server_.Stop();
        throw;
    }
    process_ = std::move(spawned.process);
    job_ = std::move(spawned.job);
    stdoutRead_ = std::move(spawned.stdoutRead);
    stderrRead_ = std::move(spawned.stderrRead);
    {
        std::lock_guard lock(writeMutex_);
        stdinWrite_ = std::move(spawned.stdinWrite);
    }
    errorReader_ = std::thread(&GrokProcess::ReadErrors, this);
    reader_ = std::thread(&GrokProcess::ReadLoop, this);
    watchdog_ = std::thread(&GrokProcess::Watchdog, this);

    Call("initialize",
         Json{{"protocolVersion", 1},
              {"clientCapabilities", {{"terminal", false}, {"fs", {{"readTextFile", false}, {"writeTextFile", false}}}}}},
         [this](const Json& result, const Json& error) {
             if (!error.is_null()) {
                 Emit(Json{{"type", "result"},
                           {"subtype", "error_during_execution"},
                           {"is_error", true},
                           {"result", "Grok: " + JsonStr(error, "message")}});
                 return;
             }
             OpenSession(result);
         });
}

void GrokProcess::Stop() {
    stopping_ = true;
    {
        std::lock_guard lock(stateMutex_);
        for (auto& [id, wait] : mcp_) {
            wait.done = true;  // the HTTP handlers return, so the server can close
        }
        queue_.clear();
    }
    stateChanged_.notify_all();
    {
        std::lock_guard lock(writeMutex_);
        stdinWrite_.reset();  // the agent's cue to finish
    }
    if (process_) {
        WaitForSingleObject(process_.get(), 800);
        TerminateJobObject(job_.get(), 1);
    }
    server_.Stop();
    if (reader_.joinable()) {
        reader_.join();
    }
    if (watchdog_.joinable()) {
        watchdog_.join();
    }
    job_.reset();
    process_.reset();
    stdoutRead_.reset();
    stderrRead_.reset();
}

void GrokProcess::SendUserText(std::string_view text) {
    SendUserContent(Json(std::string(text)));
}

void GrokProcess::SendUserContent(Json content) {
    // Claude's content blocks as ACP's.
    Json blocks = Json::array();
    if (content.is_string()) {
        blocks.push_back(Json{{"type", "text"}, {"text", content.get<std::string>()}});
    } else {
        for (const Json& block : content) {
            const std::string type = JsonStr(block, "type");
            const Json& source = JsonAt(block, "source");
            if (type == "image") {
                blocks.push_back(Json{{"type", "image"},
                                      {"data", JsonStr(source, "data")},
                                      {"mimeType", JsonStr(source, "media_type")}});
            } else if (type == "document") {
                blocks.push_back(Json{{"type", "resource"},
                                      {"resource",
                                       {{"uri", "file:///" + JsonStr(block, "title")},
                                        {"mimeType", JsonStr(source, "media_type")},
                                        {"blob", JsonStr(source, "data")}}}});
            } else {
                blocks.push_back(block);
            }
        }
    }
    {
        std::lock_guard lock(stateMutex_);
        queue_.push_back(std::move(blocks));
    }
    Pump();
}

void GrokProcess::Interrupt() {
    std::string session;
    {
        std::lock_guard lock(stateMutex_);
        queue_.clear();  // what waited for its turn goes with the one that is stopped
        session = sessionId_;
    }
    if (!session.empty()) {
        Notify("session/cancel", Json{{"sessionId", session}});
    }
}

void GrokProcess::SetModel(const std::string& model) {
    std::string session;
    {
        std::lock_guard lock(stateMutex_);
        if (!sessionOpen_) {
            pendingModel_ = model;
            return;
        }
        session = sessionId_;
    }
    Call("session/set_config_option",
         Json{{"sessionId", session}, {"configId", "model"}, {"value", {{"value", model}}}},
         [](const Json&, const Json&) {});
}

void GrokProcess::Respond(const std::string& requestId, Json response) {
    if (requestId.starts_with("mcp_")) {
        {
            std::lock_guard lock(stateMutex_);
            if (const auto it = mcp_.find(requestId); it != mcp_.end()) {
                it->second.reply = JsonAt(response, "mcp_response").dump(-1, ' ', false, Json::error_handler_t::replace);
                it->second.done = true;
            }
        }
        stateChanged_.notify_all();
        return;
    }
    Json entry;
    {
        std::lock_guard lock(stateMutex_);
        const auto it = permissions_.find(requestId);
        if (it == permissions_.end()) {
            return;
        }
        entry = std::move(it->second);
        permissions_.erase(it);
    }
    const bool allow = JsonStr(response, "behavior") == "allow";
    const bool stopped = JsonAt(response, "interrupt").is_boolean() && JsonAt(response, "interrupt").get<bool>();
    // A "no" to one of our tools is let through and answered by our MCP server with the reason: refused there,
    // Grok cancels every other call of the step and ends the turn without a word (28.10.2026, his: "оборвалось").
    const std::string tool = JsonStr(entry, "tool");
    const bool refuseInside = !allow && !stopped && !tool.empty();
    if (refuseInside) {
        std::string message = JsonStr(response, "message");
        std::lock_guard lock(stateMutex_);
        refusals_.push_back(Refusal{tool, JsonAt(entry, "input"),
                                    message.empty() ? std::string("The user declined this call.") : std::move(message)});
    }
    const char* wanted = allow || refuseInside ? "allow_once" : "reject_once";
    std::string optionId;
    for (const Json& option : JsonAt(entry, "options")) {
        if (JsonStr(option, "kind") == wanted) {
            optionId = JsonStr(option, "optionId");
        }
    }
    if (optionId.empty() || stopped) {  // ACP's answer to a prompt left when the turn is cancelled
        Reply(JsonAt(entry, "id"), Json{{"outcome", {{"outcome", "cancelled"}}}});
        return;
    }
    Reply(JsonAt(entry, "id"), Json{{"outcome", {{"outcome", "selected"}, {"optionId", optionId}}}});
}

void GrokProcess::RespondError(const std::string& requestId, std::string_view error) {
    if (requestId.starts_with("mcp_")) {
        {
            std::lock_guard lock(stateMutex_);
            if (const auto it = mcp_.find(requestId); it != mcp_.end()) {
                it->second.reply = RpcError(nullptr, -32603, std::string(error).c_str()).dump();
                it->second.done = true;
            }
        }
        stateChanged_.notify_all();
        return;
    }
    Respond(requestId, Json{{"behavior", "deny"}});
}

// ---- JSON-RPC out ----

void GrokProcess::Call(const char* method, Json params, Callback done) {
    int64_t id = 0;
    {
        std::lock_guard lock(stateMutex_);
        id = ++nextId_;
        calls_[id] = std::move(done);
    }
    WriteLine(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});
}

void GrokProcess::Notify(const char* method, Json params) {
    WriteLine(Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
}

void GrokProcess::Reply(const Json& id, Json result) {
    WriteLine(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}

void GrokProcess::WriteLine(const Json& message) {
    const std::string line = message.dump(-1, ' ', false, Json::error_handler_t::replace) + "\n";
    std::lock_guard lock(writeMutex_);
    if (!stdinWrite_) {
        return;
    }
    const char* data = line.data();
    size_t left = line.size();
    while (left > 0) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>(std::min<size_t>(left, 1 << 20));
        if (!WriteFile(stdinWrite_.get(), data, chunk, &written, nullptr)) {
            return;  // the agent is gone; the reader reports the exit
        }
        data += written;
        left -= written;
    }
}

// ---- in ----

void GrokProcess::ReadLoop() {
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
        TerminateJobObject(job_.get(), 1);
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

void GrokProcess::ReadErrors() {
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

void GrokProcess::HandleLine(std::string_view line) {
    const Json message = Json::parse(line, nullptr, false);
    if (message.is_discarded() || !message.is_object()) {
        Emit(Json{{"type", "aic_raw"}, {"text", std::string(line)}});
        return;
    }
    const std::string method = JsonStr(message, "method");
    if (method.empty()) {
        OnResponse(message);
    } else if (message.contains("id")) {
        OnRequest(message["id"], method, JsonAt(message, "params"));
    } else {
        OnNotification(method, JsonAt(message, "params"));
    }
}

void GrokProcess::OnResponse(const Json& message) {
    const Json& id = JsonAt(message, "id");
    if (!id.is_number_integer()) {
        return;
    }
    Callback done;
    {
        std::lock_guard lock(stateMutex_);
        const auto it = calls_.find(id.get<int64_t>());
        if (it == calls_.end()) {
            return;
        }
        done = std::move(it->second);
        calls_.erase(it);
    }
    if (done) {
        done(JsonAt(message, "result"), message.contains("error") ? message["error"] : Json());
    }
}

void GrokProcess::OnNotification(const std::string& method, const Json& params) {
    if (method == "session/update") {
        OnUpdate(JsonAt(params, "update"));
    } else if (method == "_x.ai/mcp_initialized") {
        MarkReady();
    }
}

void GrokProcess::OnRequest(const Json& id, const std::string& method, const Json& params) {
    if (method != "session/request_permission") {
        WriteLine(RpcError(id, -32601, "Method not found"));
        return;
    }
    const Json& call = JsonAt(params, "toolCall");
    const Json& raw = JsonAt(call, "rawInput");
    std::string name;
    Json input;
    std::string ours;  // our MCP server's name of the tool, when it is one of ours
    if (JsonStr(raw, "variant") == "UseTool") {
        const std::string tool = JsonStr(raw, "tool_name");
        name = "mcp__" + tool;  // aic__run_command -> mcp__aic__run_command
        input = JsonAt(raw, "tool_input");
        if (const std::string prefix = std::string(kToolServer) + "__"; tool.starts_with(prefix)) {
            ours = tool.substr(prefix.size());
        }
    } else {
        name = JsonStr(call, "title");  // not ours: the window refuses it
        input = raw;
    }
    std::string requestId;
    {
        std::lock_guard lock(stateMutex_);
        requestId = "perm_" + std::to_string(++permissionCounter_);
        permissions_[requestId] = Json{{"id", id}, {"options", JsonAt(params, "options")}, {"tool", ours}, {"input", input}};
        textOpen_ = false;
    }
    Emit(Json{{"type", "control_request"},
              {"request_id", requestId},
              {"request",
               {{"subtype", "can_use_tool"},
                {"tool_name", name},
                {"input", input},
                {"tool_use_id", JsonStr(call, "toolCallId")}}}});
}

void GrokProcess::OnUpdate(const Json& update) {
    const std::string type = JsonStr(update, "sessionUpdate");
    if (type == "agent_message_chunk") {
        const Json& content = JsonAt(update, "content");
        if (JsonStr(content, "type") == "text") {
            EmitText(JsonStr(content, "text"));
        }
    } else if (type == "tool_call") {
        std::lock_guard lock(stateMutex_);
        textOpen_ = false;  // what is said after the call is a new message
    } else if (type == "tool_call_update") {
        const std::string status = JsonStr(update, "status");
        if (status == "completed" || status == "failed") {
            EmitToolResult(update);
        }
    }
}

// ---- the handshake and the turns ----

void GrokProcess::OpenSession(const Json& initResult) {
    // The models, in the shape the window reads Claude's.
    Json models = Json::array();
    for (const Json& model : JsonAt(JsonAt(JsonAt(initResult, "_meta"), "modelState"), "availableModels")) {
        Json efforts = Json::array();
        for (const Json& effort : JsonAt(JsonAt(model, "_meta"), "reasoningEfforts")) {
            efforts.push_back(JsonStr(effort, "value"));
        }
        const std::string description = JsonStr(model, "description");
        models.push_back(Json{{"value", JsonStr(model, "modelId")},
                              {"displayName", JsonStr(model, "name")},
                              {"description", description},
                              {"supportedEffortLevels", std::move(efforts)}});
    }
    Emit(Json{{"type", "control_response"},
              {"response",
               {{"subtype", "success"},
                {"request_id", "aic_init"},
                {"response", {{"models", std::move(models)}, {"account", Json::object()}}}}}});

    const std::string cwd = options_.cwd.empty() ? ToUtf8(fs::current_path().wstring()) : ToUtf8(options_.cwd);
    const Json servers = Json::array(
        {Json{{"type", "http"},
              {"name", kToolServer},
              {"url", "http://127.0.0.1:" + std::to_string(serverPort_) + "/mcp"},
              {"headers", Json::array({Json{{"name", "Authorization"}, {"value", "Bearer " + token_}}})}}});
    Json meta = Json::object();
    if (!options_.appendSystemPrompt.empty()) {
        meta["rules"] = options_.appendSystemPrompt;
    }

    const auto opened = [this](const std::string& session) {
        std::string model;
        {
            std::lock_guard lock(stateMutex_);
            sessionId_ = session;
            sessionOpen_ = true;
            model = std::move(pendingModel_);
            pendingModel_.clear();
        }
        Emit(Json{{"type", "system"}, {"subtype", "init"}, {"session_id", session}});
        if (!model.empty()) {
            SetModel(model);
        }
        Pump();
    };
    const auto fresh = [this, cwd, servers, meta, opened] {
        Call("session/new", Json{{"cwd", cwd}, {"mcpServers", servers}, {"_meta", meta}},
             [this, opened](const Json& result, const Json& error) {
                 if (!error.is_null() || JsonStr(result, "sessionId").empty()) {
                     Emit(Json{{"type", "result"},
                               {"subtype", "error_during_execution"},
                               {"is_error", true},
                               {"result", "Grok: " + (error.is_null() ? std::string("no session") : JsonStr(error, "message"))}});
                     return;
                 }
                 opened(JsonStr(result, "sessionId"));
             });
    };
    if (options_.resume.empty()) {
        fresh();
        return;
    }
    Call("session/resume", Json{{"sessionId", options_.resume}, {"cwd", cwd}, {"mcpServers", servers}, {"_meta", meta}},
         [this, fresh, opened](const Json& /*result*/, const Json& error) {
             if (!error.is_null()) {
                 Emit(Json{{"type", "aic_raw"},
                           {"text", "Grok не смог продолжить прошлую сессию (" + JsonStr(error, "message") +
                                        "): начат новый чат."}});
                 fresh();
                 return;
             }
             opened(options_.resume);
         });
}

void GrokProcess::MarkReady() {
    {
        std::lock_guard lock(stateMutex_);
        mcpReady_ = true;
    }
    stateChanged_.notify_all();
    Pump();
}

void GrokProcess::Watchdog() {
    std::unique_lock lock(stateMutex_);
    const bool ready = stateChanged_.wait_for(lock, kMcpWait, [this] { return mcpReady_ || stopping_; });
    lock.unlock();
    if (!ready) {
        MarkReady();  // Grok never said it connected: ask anyway, a tool call will say what is wrong
    }
}

void GrokProcess::Pump() {
    Json prompt;
    std::string session;
    {
        std::lock_guard lock(stateMutex_);
        if (!sessionOpen_ || !mcpReady_ || turnActive_ || queue_.empty()) {
            return;
        }
        prompt = std::move(queue_.front());
        queue_.pop_front();
        turnActive_ = true;
        textOpen_ = false;
        session = sessionId_;
    }
    EmitState("running");
    Call("session/prompt", Json{{"sessionId", session}, {"prompt", std::move(prompt)}},
         [this](const Json& result, const Json& error) { FinishTurn(result, error); });
}

void GrokProcess::FinishTurn(const Json& result, const Json& error) {
    bool more = false;
    {
        std::lock_guard lock(stateMutex_);
        turnActive_ = false;
        textOpen_ = false;
        refusals_.clear();  // a "no" whose call never came: not carried to the next turn
        more = !queue_.empty();
    }
    Json out{{"type", "result"}, {"subtype", "success"}, {"is_error", false}};
    if (!error.is_null()) {
        out["subtype"] = "error_during_execution";
        out["is_error"] = true;
        out["result"] = JsonStr(error, "message");
    } else {
        // The last request of the turn is the chat's size now ("Auto" and the context ring read it).
        const Json& meta = JsonAt(result, "_meta");
        const long long input = Number(meta, "inputTokens");
        const long long cached = Number(meta, "cachedReadTokens");
        out["usage"] = Json{{"input_tokens", std::max(0LL, input - cached)},
                            {"cache_read_input_tokens", cached},
                            {"cache_creation_input_tokens", 0},
                            {"output_tokens", Number(meta, "outputTokens")}};
        const Json& ticks = JsonAt(JsonAt(meta, "usage"), "costUsdTicks");
        if (ticks.is_number()) {
            out["total_cost_usd"] = ticks.get<double>() / static_cast<double>(kTicksPerDollar);
        }
    }
    Emit(std::move(out));
    if (more) {
        Pump();
    } else {
        EmitState("idle");
    }
}

void GrokProcess::Emit(Json message) {
    if (!stopping_ && onMessage_) {
        onMessage_(std::move(message));
    }
}

void GrokProcess::EmitState(const char* state) {
    Emit(Json{{"type", "system"}, {"subtype", "session_state_changed"}, {"state", state}});
}

void GrokProcess::EmitText(const std::string& text) {
    if (text.empty()) {
        return;
    }
    bool opening = false;
    {
        std::lock_guard lock(stateMutex_);
        if (!textOpen_) {
            textOpen_ = true;
            opening = true;
            messageId_ = "grok_" + std::to_string(++messageCounter_);
        }
    }
    if (opening) {
        Emit(Json{{"type", "stream_event"}, {"event", {{"type", "message_start"}, {"message", {{"id", messageId_}}}}}});
        Emit(Json{{"type", "stream_event"},
                  {"event",
                   {{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "text"}, {"text", ""}}}}}});
    }
    Emit(Json{{"type", "stream_event"},
              {"event",
               {{"type", "content_block_delta"}, {"index", 0}, {"delta", {{"type", "text_delta"}, {"text", text}}}}}});
}

void GrokProcess::EmitToolResult(const Json& update) {
    const std::string text = OutputText(JsonAt(update, "rawOutput"));
    Emit(Json{{"type", "user"},
              {"message",
               {{"role", "user"},
                {"content",
                 Json::array({Json{{"type", "tool_result"},
                                   {"tool_use_id", JsonStr(update, "toolCallId")},
                                   {"content", Json::array({Json{{"type", "text"}, {"text", text}}})},
                                   {"is_error", JsonStr(update, "status") == "failed"}}})}}}});
}

// Grok's requests to our MCP server, on a connection's thread: handed to the window as Claude's mcp_message and
// held until ToolHost has the answer.
std::string GrokProcess::HandleMcpRequest(const std::string& body) {
    const Json request = Json::parse(body, nullptr, false);
    if (request.is_discarded() || !request.is_object()) {
        return RpcError(nullptr, -32700, "Parse error").dump();
    }
    if (!request.contains("id")) {
        return {};  // a notification
    }
    const Json& id = request["id"];
    if (JsonStr(request, "method") == "server/discover") {
        return RpcError(id, -32601, "Method not found").dump();  // Grok falls back to initialize
    }
    std::string requestId;
    {
        std::lock_guard lock(stateMutex_);
        if (JsonStr(request, "method") == "tools/call" && !refusals_.empty()) {
            // A call the user said no to: the same tool with the same input, or, failing that, the same tool
            // (when in doubt it is refused, never run).
            const Json& params = JsonAt(request, "params");
            const std::string tool = JsonStr(params, "name");
            auto it = std::ranges::find_if(refusals_, [&](const Refusal& refusal) {
                return refusal.tool == tool && refusal.input == JsonAt(params, "arguments");
            });
            if (it == refusals_.end()) {
                it = std::ranges::find_if(refusals_, [&](const Refusal& refusal) { return refusal.tool == tool; });
            }
            if (it != refusals_.end()) {
                Json result{{"content", Json::array({Json{{"type", "text"}, {"text", it->message}}})}, {"isError", true}};
                refusals_.erase(it);
                return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}.dump(
                    -1, ' ', false, Json::error_handler_t::replace);
            }
        }
        requestId = "mcp_" + std::to_string(++mcpCounter_);
        mcp_[requestId] = McpWait{};
    }
    Emit(Json{{"type", "control_request"},
              {"request_id", requestId},
              {"request", {{"subtype", "mcp_message"}, {"server_name", kToolServer}, {"message", request}}}});
    std::string reply;
    {
        std::unique_lock lock(stateMutex_);
        stateChanged_.wait(lock, [&] { return mcp_[requestId].done || stopping_; });
        reply = std::move(mcp_[requestId].reply);
        mcp_.erase(requestId);
    }
    return reply.empty() ? RpcError(id, -32603, "The client is closing").dump() : reply;
}

std::wstring FindGrokExe() {
    std::array<wchar_t, 2048> buffer{};
    const auto size = static_cast<DWORD>(buffer.size());
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer.data(), size);
    if (length > 0 && length < size) {
        std::wstring candidate = std::wstring(buffer.data(), length) + L"\\.grok\\bin\\grok.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    length = SearchPathW(nullptr, L"grok.exe", nullptr, size, buffer.data(), nullptr);
    if (length > 0 && length < size) {
        return std::wstring(buffer.data(), length);
    }
    return {};
}

}  // namespace aic
