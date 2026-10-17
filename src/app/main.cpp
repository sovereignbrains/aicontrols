#include <windows.h>

#include "app/chat_window.h"

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int show) {
    return aic::RunChatWindow(instance, show);
}
