// Chats moved between projects (their history folders under ~\.claude\projects, made for two scratch
// workspaces and removed after). Exit code 0: all passed.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "core/chat_export.h"
#include "core/store.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path();
    const std::wstring a = (temp / L"aic_store_test_a").wstring();
    const std::wstring b = (temp / L"aic_store_test_b").wstring();
    // Where the CLI keeps their chats: the memory folder's parent.
    const fs::path historyA = fs::path(aic::MemoryFolder(a)).parent_path();
    const fs::path historyB = fs::path(aic::MemoryFolder(b)).parent_path();
    std::error_code error;
    fs::remove_all(historyA, error);
    fs::remove_all(historyB, error);
    fs::create_directories(historyA / L"chat-1", error);
    std::ofstream(historyA / L"chat-1.jsonl") << R"({"type":"user","message":{"role":"user","content":"hello"}})" << "\n";
    std::ofstream(historyA / L"chat-1" / L"result.txt") << "x";

    Expect(aic::ListSessions(a).size() == 1 && aic::ListSessions(b).empty(), "the chat is in the first project");
    Expect(aic::MoveSession(a, b, "chat-1"), "moved");
    Expect(aic::ListSessions(a).empty() && aic::ListSessions(b).size() == 1 && aic::ListSessions(b)[0].id == "chat-1",
           "now it is in the second");
    Expect(fs::exists(historyB / L"chat-1" / L"result.txt") && !fs::exists(historyA / L"chat-1"),
           "its folder went with it");
    Expect(aic::ListSessions(b)[0].title == L"hello", "its title as before");
    Expect(!aic::MoveSession(a, b, "chat-1"), "a chat that is not there is not moved");
    Expect(!aic::MoveSession(b, b, "chat-1"), "nor to its own project");
    Expect(!aic::MoveSession(b, a, "..\\chat-1"), "nor by a path");

    std::ofstream(historyA / L"chat-1.jsonl") << "{}\n";  // the same id waiting there: nothing is overwritten
    Expect(!aic::MoveSession(b, a, "chat-1") && fs::exists(historyB / L"chat-1.jsonl"), "no chat is overwritten");

    // A compacted chat: a mark where it was summed up, and the CLI's summary is not shown as the user's words.
    std::ofstream(historyA / L"chat-2.jsonl")
        << R"({"type":"user","message":{"role":"user","content":"remember pelican"}})" << "\n"
        << R"({"type":"system","subtype":"compact_boundary","compact_metadata":{"pre_tokens":3972,"post_tokens":743}})" << "\n"
        << R"({"type":"user","isCompactSummary":true,"message":{"role":"user","content":"This session is being continued"}})" << "\n"
        << R"({"type":"user","message":{"role":"user","content":"which word?"}})" << "\n";
    bool truncated = false;
    const std::vector<aic::TranscriptEntry> entries = aic::LoadTranscript(a, "chat-2", 100, truncated);
    Expect(entries.size() == 3 && entries[0].kind == aic::EntryKind::User && entries[1].kind == aic::EntryKind::Compacted &&
               entries[2].kind == aic::EntryKind::User && entries[2].text == L"which word?",
           "a compacted chat: the mark, no summary");

    // Grok's chats: a folder each under ~\.grok\sessions\<workspace, %-encoded>, listed beside Claude's.
    const fs::path grokA = aic::GrokHistoryFolder(a);
    Expect(grokA.filename().wstring().ends_with(L"%5Caic_store_test_a"), "grok's folder name: the path %-encoded");
    fs::remove_all(grokA, error);
    fs::create_directories(grokA / L"g-1", error);
    fs::create_directories(grokA / L"g-empty", error);
    std::ofstream(grokA / L"g-1" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<rules>be brief</rules>"}]})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<image_files>\n1.png\n</image_files>\n\n<user_query>\n)"
        << R"(сделай навигацию\nи логику\n</user_query>"},{"type":"image","url":"data:image/png;base64,iVBO"}],"prompt_index":0})" << "\n"
        << R"({"type":"reasoning","content":"thinking"})" << "\n"
        << R"({"type":"assistant","content":"","tool_calls":[{"id":"c1","name":"search_tool","arguments":"{}"},)"
        << R"({"id":"c2","name":"use_tool","arguments":"{\"tool_name\":\"aic__read_file\",\"tool_input\":{\"path\":\"a.txt\"}}"}]})" << "\n"
        << R"({"type":"tool_result","tool_call_id":"c2","content":"hello\n"})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<system-reminder>x</system-reminder>"}],"synthetic_reason":"r"})" << "\n"
        << R"({"type":"assistant","content":"done"})" << "\n";
    fs::create_directories(grokA / L"g-2", error);
    std::ofstream(grokA / L"g-2" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\nнарисуй рыбу\n</user_query>"}],"prompt_index":0})" << "\n"
        << R"({"type":"backend_tool_call","kind":{"tool_type":"web_search","action":{"type":"search","query":"fish"},"status":"completed"}})" << "\n"
        << R"({"type":"assistant","content":"","tool_calls":[{"id":"i1","name":"image_gen","arguments":"{\"prompt\":\"a fish\"}"},)"
        << R"({"id":"i2","name":"image_to_video","arguments":"{\"prompt\":\"swims\"}"}]})" << "\n"
        << R"({"type":"tool_result","tool_call_id":"i1","content":"{\"path\":\"C:\\\\1.jpg\"}"})" << "\n"
        << R"({"type":"tool_result","tool_call_id":"i2","content":"Tool `image_to_video` failed: no video"})" << "\n";
    std::ofstream(grokA / L"g-empty" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<rules>be brief</rules>"}]})" << "\n";
    // Compacted twice (grok 1.0.62): the turns before each compaction in a segment, the history left with Grok's rules
    // (compaction_meta), the prompt the compaction fell into, without its prompt_index, and what came after.
    fs::create_directories(grokA / L"g-3" / L"compaction", error);
    std::ofstream(grokA / L"g-3" / L"compaction" / L"segment_000.md") << R"seg(# HISTORICAL -- DO NOT EDIT

## Summary (curated by compaction step)
<user_query>
not a turn
</user_query>

## Verbatim turns

### Turn 0 (System)
You are Grok.

### Turn 1 (Human)
<user_info>
OS Version: windows
</user_info>

### Turn 2 (Human)
<user_query>
почему пропал чат
</user_query>

### Turn 3 (Assistant)
Сейчас посмотрю.
[tool_request: use_tool]
- tool_name: aic__read_file
- tool_input: {"path":"a.txt"}

### Turn 4 (Function)
[tool_response]
hello

### Turn 5 (Human)
<image_files>
1.png
</image_files>

<user_query>
вот снимок
</user_query>
[image]
)seg";
    std::ofstream(grokA / L"g-3" / L"compaction" / L"segment_001.md") << R"seg(## Verbatim turns

### Turn 0 (System)
You are Grok.

### Turn 1 (Human)
<user_query>
вот снимок
</user_query>
[image]

### Turn 2 (Human)
This session is being continued

### Turn 3 (Assistant)
Нашёл.

### Turn 4 (Human)
<user_query>
чини
</user_query>
)seg";
    std::ofstream(grokA / L"g-3" / L"chat_history.jsonl")
        << R"({"type":"system","content":"You are Grok."})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<user_info>x</user_info>"}],"synthetic_reason":"compaction_meta"})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\nчини\n</user_query>"}]})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"This session is being continued"}],"synthetic_reason":"compaction_meta"})" << "\n"
        << R"({"type":"assistant","content":"починил"})" << "\n";
    fs::create_directories(grokA / L"g-4", error);  // compacted, its segments gone
    std::ofstream(grokA / L"g-4" / L"chat_history.jsonl")
        << R"({"type":"user","content":[{"type":"text","text":"<user_info>x</user_info>"}],"synthetic_reason":"compaction_meta"})" << "\n"
        << R"({"type":"user","content":[{"type":"text","text":"<user_query>\nдальше\n</user_query>"}]})" << "\n"
        << R"({"type":"assistant","content":"ок"})" << "\n";
    std::vector<aic::SessionInfo> listed = aic::ListSessions(a);
    const auto compacted = std::ranges::find(listed, std::string("g-3"), &aic::SessionInfo::id);
    Expect(compacted != listed.end() && compacted->grok && compacted->title == L"почему пропал чат",
           "a compacted grok chat: listed, named after its first message in the first segment");
    const auto lost = std::ranges::find(listed, std::string("g-4"), &aic::SessionInfo::id);
    Expect(lost != listed.end() && lost->title == L"дальше", "a compacted grok chat without segments: the prompt it kept");
    const std::vector<aic::TranscriptEntry> whole = aic::LoadTranscript(a, "g-3", 100, truncated);
    const std::vector<aic::EntryKind> kinds = {aic::EntryKind::User,      aic::EntryKind::Assistant, aic::EntryKind::User,
                                               aic::EntryKind::Compacted, aic::EntryKind::Assistant, aic::EntryKind::User,
                                               aic::EntryKind::Compacted, aic::EntryKind::Assistant};
    Expect(std::ranges::equal(whole, kinds, {}, &aic::TranscriptEntry::kind) && whole[0].text == L"почему пропал чат" &&
               whole[1].text == L"Сейчас посмотрю." && whole[2].text == L"вот снимок\n\n📎 фото" &&
               whole[4].text == L"Нашёл." && whole[5].text == L"чини" && whole[7].text == L"починил",
           "a compacted grok transcript: the segments' words, a mark at each compaction, its prompt once");
    const std::vector<aic::TranscriptEntry> kept = aic::LoadTranscript(a, "g-4", 100, truncated);
    Expect(kept.size() == 3 && kept[0].kind == aic::EntryKind::Compacted && kept[1].text == L"дальше" && kept[2].text == L"ок",
           "a compacted grok transcript without segments: the mark, then what is left");
    const auto grokChat = std::ranges::find(listed, std::string("g-1"), &aic::SessionInfo::id);
    Expect(grokChat != listed.end() && grokChat->grok && grokChat->title == L"сделай навигацию",
           "a grok chat: listed, named after its first message");
    Expect(std::ranges::find(listed, std::string("g-empty"), &aic::SessionInfo::id) == listed.end(),
           "a grok session nothing was said in is not listed");
    const std::vector<aic::TranscriptEntry> said = aic::LoadTranscript(a, "g-1", 100, truncated);
    Expect(said.size() == 3 && said[0].kind == aic::EntryKind::User && said[0].text == L"сделай навигацию\nи логику" &&
               said[0].images.size() == 1 && said[0].images[0].mediaType == "image/png",
           "a grok transcript: the words as typed, the picture");
    Expect(said.size() == 3 && said[1].kind == aic::EntryKind::Tool && said[1].text == L"mcp__aic__read_file" &&
               said[1].toolInput == R"({"path":"a.txt"})" && said[1].toolOutput == L"hello" && said[1].toolAnswered,
           "a grok tool call as it comes live, with its answer; its tool lookups left out");
    Expect(said.size() == 3 && said[2].kind == aic::EntryKind::Assistant && said[2].text == L"done", "the answer");
    const std::vector<aic::TranscriptEntry> made = aic::LoadTranscript(a, "g-2", 100, truncated);
    Expect(made.size() == 4 && made[1].text == L"grok__web_search" && made[1].toolInput.find("\"fish\"") != std::string::npos &&
               made[2].text == L"grok__image_gen" && made[2].toolOutput == LR"({"path":"C:\\1.jpg"})" && !made[2].toolFailed &&
               made[3].text == L"grok__image_to_video" && made[3].toolFailed,
           "grok's own tools in its history: the search, the picture, the video that failed");
    Expect(aic::RenameSession(a, "g-1", L"Навигация") && aic::SessionTitle(a, "g-1") == L"Навигация" &&
               fs::exists(grokA / L"g-1" / L"chat_history.jsonl"),
           "a grok chat renamed, its history left as it is");
    Expect(!aic::RenameSession(a, "..\\g-1", L"x") && !aic::MoveSession(a, b, "g-1"), "no paths; grok's are not moved");
    const std::optional<std::string> markdown = aic::ChatMarkdown(a, "g-1");
    Expect(markdown && markdown->starts_with("# Навигация\n") && markdown->find("## Grok") != std::string::npos &&
               markdown->find("Прочитать файл") != std::string::npos && markdown->find("```\nhello\n```") != std::string::npos,
           "a chat as Markdown: its title, who said what, the tool's card and output");
    Expect(!aic::ChatMarkdown(a, "nope"), "no Markdown for a chat that is not there");
    fs::remove_all(grokA, error);

    // The sidebar's order: the open project first when sorting by recent, the general chats always last.
    const std::wstring general = L"general";
    const std::vector<aic::ProjectPlace> places = {
        {L"sov", L"Sovereign", 10},
        {general, L"Obshchee", 100},
        {L"dpi", L"dpiprobe", 50},
        {L"aic", L"aicontrols", 50},
        {L"old", L"Windows", 0},
    };
    const auto foldersOf = [](const std::vector<aic::ProjectPlace>& list) {
        std::vector<std::wstring> names;
        for (const aic::ProjectPlace& place : list) {
            names.push_back(place.folder);
        }
        return names;
    };
    Expect(foldersOf(aic::OrderProjects(places, general, L"sov", aic::ProjectOrder::Recent)) ==
               std::vector<std::wstring>{L"sov", L"dpi", L"aic", L"old", general},
           "recent: the open project, then the newest, ties keep the added order, general last");
    Expect(foldersOf(aic::OrderProjects(places, general, L"aic", aic::ProjectOrder::Name)) ==
               std::vector<std::wstring>{L"aic", L"dpi", L"sov", L"old", general},
           "name: A to Z, ignoring case, general still last");
    Expect(foldersOf(aic::OrderProjects(places, general, L"sov", aic::ProjectOrder::Added)) ==
               std::vector<std::wstring>{L"sov", L"dpi", L"aic", L"old", general},
           "added: the given order, general pulled out to the end");
    Expect(aic::ParseProjectOrder("") == aic::ProjectOrder::Recent &&
               aic::ParseProjectOrder("nope") == aic::ProjectOrder::Recent &&
               aic::ParseProjectOrder("added") == aic::ProjectOrder::Added &&
               aic::ParseProjectOrder("name") == aic::ProjectOrder::Name,
           "an unknown order is recent");
    Expect(std::string(aic::ProjectOrderId(aic::ProjectOrder::Added)) == "added" &&
               std::string(aic::ProjectOrderId(aic::ProjectOrder::Name)) == "name",
           "the order is stored by those names");

    fs::remove_all(historyA, error);
    fs::remove_all(historyB, error);
    if (failures == 0) {
        std::cout << "store: all passed\n";
    }
    return failures == 0 ? 0 : 1;
}
