// A chat as Markdown: a file to send someone, and what read_chat gives an agent.
#pragma once

#include <optional>
#include <string>

#include "core/store.h"

namespace aic {

// The whole chat `id` of a workspace: a heading, then each turn under who said it (the user, Claude or Grok); a tool
// call as its card (the title, the detail) with the start of what it printed; pictures as a mark. nullopt: no such chat.
std::optional<std::string> ChatMarkdown(const std::wstring& workspace, const std::string& id);

// Where a chat is saved to be sent: Documents\AIControls\Чаты\<its title> <date>.md.
std::wstring ChatExportPath(const SessionInfo& session);

// "28.10.2026 14:05", local time.
std::string ChatTime(const FILETIME& time);

}  // namespace aic
