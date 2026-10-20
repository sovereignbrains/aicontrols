// By hand: how the chats of the default workspace are titled (how many still have none).
#include <iostream>

#include "core/claude_process.h"
#include "core/store.h"

int main() {
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
