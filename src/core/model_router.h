#pragma once

#include <string>
#include <string_view>

namespace aic {

// "Auto" in the model menu: which model a message goes to (22.10.2026, his: the limits melt, and he would not
// choose by hand). The rules look at the words, not the meaning, so they are kept few and sticky: a switch makes
// the new model write the whole chat into its cache again (measured: the full context, once per switch).
struct RouteInput {
    std::wstring_view text;    // the message as typed
    size_t attachments = 0;    // pictures and files with it
    bool first = false;        // the chat's first message
    std::string current;       // the alias in use: "opus", "sonnet", "haiku"
    long long context = 0;     // tokens the chat holds now (the last turn's input); 0: unknown
};

// "opus", "sonnet" or "haiku".
// - A short reply ("го", "да", "давай") goes on with the model in use: it continues the task.
// - A task that needs thought (design, "не работает", a review, a rewrite, a very long message, many files): Opus.
// - A chat that starts with a short question about no code nor files: Haiku; once code comes up, Sonnet.
// - Anything else: Sonnet. Down from Opus only while the chat is small, as the switch rewrites its cache.
std::string RouteModel(const RouteInput& input);

}  // namespace aic
