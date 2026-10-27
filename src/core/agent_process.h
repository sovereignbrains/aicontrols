#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace aic {

using Json = nlohmann::json;

// What the chat window needs of the CLI behind a chat. Every provider speaks to it in Claude's stream-json
// messages (system / stream_event / assistant / user / result / control_request …): a provider whose CLI
// speaks something else translates.
class AgentProcess {
public:
    using MessageHandler = std::function<void(Json)>;
    using ExitHandler = std::function<void(DWORD exitCode, std::string stderrTail)>;

    virtual ~AgentProcess() = default;

    virtual void Stop() = 0;
    virtual void SendUserText(std::string_view text) = 0;
    // A message as content blocks: pictures, documents, text.
    virtual void SendUserContent(Json content) = 0;
    virtual void Interrupt() = 0;
    // Another model for the turns to come, the conversation kept (no restart).
    virtual void SetModel(const std::string& model) = 0;
    virtual void Respond(const std::string& requestId, Json response) = 0;
    virtual void RespondError(const std::string& requestId, std::string_view error) = 0;
};

}  // namespace aic
