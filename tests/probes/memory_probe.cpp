// The header's "save the memory" by hand, on the default workspace's real memory.
//   aic_memory_probe                 push the memory folder (GITHUB in the environment)
//   aic_memory_probe --dry <session> ask Haiku what to remember from that chat and print it; nothing is written
#include <windows.h>

#include <iostream>
#include <string>

#include "core/github.h"
#include "core/memory_writer.h"
#include "core/store.h"

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc == 3 && std::string(argv[1]) == "--dry") {
        aic::MemoryEdits edits;
        const aic::MemoryWrite written = aic::WriteMemoryFromChat(aic::DefaultWorkspace(), argv[2], nullptr, &edits);
        std::cout << "ok=" << written.ok << " error=" << written.error << "\n";
        for (const aic::MemoryAppend& append : edits.appends) {
            std::cout << "\n+= " << append.file << ":\n" << append.text << "\n";
        }
        for (const aic::MemoryNote& note : edits.notes) {
            std::cout << "\nnew " << note.file << " (" << note.type << "): " << note.description << "\n"
                      << note.body << "\nindex: " << note.index << "\n";
        }
        return written.ok ? 0 : 1;
    }
    char token[512]{};
    if (GetEnvironmentVariableA("GITHUB", token, sizeof(token)) == 0) {
        std::cerr << "set GITHUB\n";
        return 2;
    }
    const aic::MemorySave saved = aic::SaveMemory(aic::MemoryFolder(aic::DefaultWorkspace()), token);
    std::cout << "ok=" << saved.ok << " nothing=" << saved.nothing << "\n" << saved.output << "\n";
    return saved.ok ? 0 : 1;
}
