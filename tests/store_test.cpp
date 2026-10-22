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

    fs::remove_all(historyA, error);
    fs::remove_all(historyB, error);
    if (failures == 0) {
        std::cout << "store: all passed\n";
    }
    return failures == 0 ? 0 : 1;
}
