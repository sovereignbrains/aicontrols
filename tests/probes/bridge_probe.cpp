// By hand: the bridge (core/bridge.h) listening for `seconds` (default 60), its URL printed, for curl -x to go
// through; --no-direct: as with the VPN off, so the way is sing-box's port or the SSH tunnel to a stored server.
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "core/bridge.h"
#include "core/claude_process.h"

int wmain(int argc, wchar_t** argv) {
    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--no-direct") {
            aic::BridgeWithoutDirect();
        } else {
            seconds = std::stoi(argv[i]);
        }
    }
    const std::wstring url = aic::BridgeUrl();
    std::cout << (url.empty() ? std::string("cannot listen") : aic::ToUtf8(url)) << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    return url.empty() ? 1 : 0;
}
