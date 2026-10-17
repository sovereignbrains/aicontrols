#pragma once

#include <windows.h>

namespace aic {

// The main window: the chat feed, the input below it. Runs the message loop until it closes.
int RunChatWindow(HINSTANCE instance, int show);

}  // namespace aic
