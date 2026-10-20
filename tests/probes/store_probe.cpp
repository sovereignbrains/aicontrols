// By hand: how the chats of the default workspace are titled (how many still have none).
#include <iostream>
#include <string>

#include "core/claude_process.h"
#include "core/store.h"

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring(argv[1]) == L"--delete") {  // deletes one chat by id (to the Recycle Bin)
        const bool ok = argc > 2 && aic::DeleteSession(aic::DefaultWorkspace(), aic::ToUtf8(argv[2]));
        std::cout << (ok ? "deleted\n" : "not deleted\n");
        return ok ? 0 : 1;
    }
    const auto sessions = aic::ListSessions(aic::DefaultWorkspace());
    size_t untitled = 0;
    for (const auto& session : sessions) {
        if (session.title.empty()) {
            ++untitled;
        }
    }
    std::cout << "chats: " << sessions.size() << ", untitled: " << untitled << "\n";
    for (size_t i = 0; i < sessions.size() && i < 12; ++i) {
        std::cout << "  " << aic::ToUtf8(sessions[i].title.empty() ? L"(none)" : sessions[i].title) << "\n";
    }
    return 0;
}
