// "Auto" model rules (core/model_router). Exit code 0: all passed.
#include <iostream>
#include <string>

#include "core/model_router.h"

namespace {

int failures = 0;

void Expect(const std::string& got, const std::string& want, const char* what) {
    if (got != want) {
        ++failures;
        std::cerr << "FAIL: " << what << ": " << got << " instead of " << want << "\n";
    }
}

std::string Route(const wchar_t* text, const char* current, bool first = false, size_t attachments = 0,
                  long long context = 0) {
    return aic::RouteModel({text, attachments, first, current, context});
}

}  // namespace

int main() {
    // A chat that starts small and stays words.
    Expect(Route(L"что значит слово эвристика?", "sonnet", true), "haiku", "a first short question");
    Expect(Route(L"а пример?", "haiku"), "haiku", "a reply in Haiku's chat");
    Expect(Route(L"а как это в коде?", "haiku"), "sonnet", "code comes up: up to Sonnet");

    // Work.
    Expect(Route(L"почини баг в файле chat_window.cpp", "sonnet", true), "sonnet", "a first message about code");
    Expect(Route(L"го", "opus"), "opus", "a short reply goes on with Opus");
    Expect(Route(L"да давай", "sonnet"), "sonnet", "a short reply goes on with Sonnet");
    Expect(Route(L"Не работает кнопка обновления", "sonnet"), "opus", "'не работает' asks for thought");
    Expect(Route(L"спроектируй хранилище", "haiku", true), "opus", "design: Opus");
    Expect(Route(std::wstring(1600, L'x').c_str(), "sonnet"), "opus", "a very long message");
    Expect(Route(L"вот", "sonnet", false, 3), "opus", "many attachments");
    Expect(Route(L"глянь скрин", "haiku", false, 1), "sonnet", "a picture is a task");

    // Down from Opus only while the chat is small.
    const std::wstring task(500, L'a');
    Expect(Route(task.c_str(), "opus", false, 0, 20'000), "sonnet", "a new task in a small chat: down to Sonnet");
    Expect(Route(task.c_str(), "opus", false, 0, 120'000), "opus", "a new task in a big chat: Opus stays");

    if (failures == 0) {
        std::cout << "model_router: all passed\n";
    }
    return failures == 0 ? 0 : 1;
}
