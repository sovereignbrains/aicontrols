#include "core/model_router.h"

#include <windows.h>

#include <array>

namespace aic {
namespace {

// Words that ask for thought rather than for doing: the task goes to Opus.
constexpr std::array<std::wstring_view, 14> kThink = {
    L"спроектир", L"архитектур", L"разберись", L"разберём", L"разобраться", L"не работает", L"найди причину",
    L"ревью", L"перепиши", L"рефактор", L"продумай", L"методик", L"почему падает", L"почему не"};

// Code, files and commands: not a chat for Haiku.
constexpr std::array<std::wstring_view, 22> kCode = {
    L"код",    L"функци", L"файл",   L"собер",  L"сборк",  L"запуст", L"баг",   L"ошибк",
    L"коммит", L"скрипт", L"сервер", L"команд", L"проект", L"клиент", L"git",   L".exe",
    L".cpp",   L"json",   L"html",   L"api",    L"`",      L":\\"};

constexpr size_t kShort = 200;      // up to this, a first message may be a chat for Haiku
constexpr size_t kTask = 400;       // from this, a message is a new task, not a reply
constexpr size_t kLong = 1500;      // from this, a task for Opus
constexpr long long kSmallChat = 50'000;  // below this, a switch down rewrites little

std::wstring Lower(std::wstring_view text) {
    std::wstring lower(text);
    if (!lower.empty()) {
        CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
    }
    return lower;
}

template <size_t N>
bool HasAny(const std::wstring& text, const std::array<std::wstring_view, N>& words) {
    for (const std::wstring_view word : words) {
        if (text.find(word) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string RouteModel(const RouteInput& input) {
    const std::string current = input.current.empty() ? "sonnet" : input.current;
    const std::wstring text = Lower(input.text);
    const size_t length = input.text.size();
    const bool think = length >= kLong || input.attachments >= 3 || HasAny(text, kThink);
    const bool code = HasAny(text, kCode);
    if (think) {
        return "opus";
    }
    const bool task = input.first || length >= kTask || input.attachments > 0;
    if (!task) {  // a reply goes on with the model of its task; code in a chat Haiku had: up to Sonnet
        return current == "haiku" && code ? "sonnet" : current;
    }
    if (input.first && length < kShort && input.attachments == 0 && !code) {
        return "haiku";
    }
    if (current == "opus" && input.context > kSmallChat) {
        return "opus";  // down now would make Sonnet write the whole big chat into its cache
    }
    return "sonnet";
}

}  // namespace aic
