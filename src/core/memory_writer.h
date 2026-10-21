#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace aic {

// Claude's memory written from a chat by one cheap call: Haiku, no tools, a short system prompt, the chat's
// words only (no tool output, the end of it) and the memory's index; it answers with what to add, the client
// writes it. Then SaveMemory pushes it.

struct MemoryNote {        // a new file
    std::string file;      // project_x.md
    std::string name;      // its kebab-case slug
    std::string description;
    std::string type;      // user, feedback, project, reference
    std::string body;
    std::string index;     // its line for MEMORY.md
};

struct MemoryAppend {      // more for a file that is there
    std::string file;
    std::string text;
};

struct MemoryEdits {
    std::vector<MemoryNote> notes;
    std::vector<MemoryAppend> appends;
};

// What the model is asked; `chat` is the conversation as plain text.
std::string MemoryPrompt(const std::wstring& memoryFolder, const std::string& chat);
// Its answer (JSON, perhaps in a code fence) into edits; anything unsafe (a bad file name, a secret) is dropped.
MemoryEdits ParseMemoryEdits(const std::string& answer);
// Writes them; the number of files created and added to.
void ApplyMemoryEdits(const std::wstring& memoryFolder, const MemoryEdits& edits, int& created, int& appended);

struct MemoryWrite {
    bool ok = false;
    int created = 0;
    int appended = 0;
    std::string error;
};
// The whole of it for one chat of a workspace. With preview, the edits go there and nothing is written.
MemoryWrite WriteMemoryFromChat(const std::wstring& workspace, const std::string& sessionId,
                                const std::atomic<bool>* cancel, MemoryEdits* preview = nullptr);

}  // namespace aic
