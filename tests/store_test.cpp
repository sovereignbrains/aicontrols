// Chats moved between projects (their history folders under ~\.claude\projects, made for two scratch
// workspaces and removed after). Exit code 0: all passed.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
