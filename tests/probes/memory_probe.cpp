// Saves the real memory folder of the default workspace to its repository, as the header's button does.
// The token comes from the GITHUB environment variable. Usage: aic_memory_probe
#include <windows.h>

#include <iostream>
#include <string>

#include "core/github.h"
#include "core/store.h"

int main() {
    char token[512]{};
    if (GetEnvironmentVariableA("GITHUB", token, sizeof(token)) == 0) {
        std::cerr << "set GITHUB\n";
        return 2;
    }
    const std::wstring folder = aic::MemoryFolder(aic::DefaultWorkspace());
    const aic::MemorySave saved = aic::SaveMemory(folder, token);
    std::cout << "ok=" << saved.ok << " nothing=" << saved.nothing << "\n" << saved.output << "\n";
    return saved.ok ? 0 : 1;
}
