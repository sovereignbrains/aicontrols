#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <wil/resource.h>

namespace aic {

// A console program (PowerShell) in a Windows pseudo console: its output arrives as VT bytes, keys are
// written to it as VT bytes. One program per object.
class Pty {
public:
    using DataHandler = std::function<void(std::string)>;  // on the reader thread
    using ExitHandler = std::function<void(DWORD exitCode)>;  // on the reader thread, unless Stop() ended it

    Pty() = default;
    ~Pty();
    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // Throws std::runtime_error when the program cannot start.
    void Start(int cols, int rows, const std::wstring& commandLine, const std::wstring& cwd, DataHandler onData,
               ExitHandler onExit);
    // Ends the program (and what it started) and the reader; no handler runs after this returns.
    void Stop();

    void Write(std::string_view bytes);
    void Resize(int cols, int rows);
    bool Running() const { return pseudoConsole_ != nullptr && !stopping_; }

private:
    void ReadLoop();
    void WaitLoop();

    HPCON pseudoConsole_ = nullptr;
    wil::unique_handle process_;
    wil::unique_handle job_;
    wil::unique_handle input_;   // the program's stdin: we write
    wil::unique_handle output_;  // its stdout: the reader reads
    std::thread reader_;
    std::thread waiter_;
    std::mutex writeMutex_;
    std::atomic<bool> stopping_ = false;
    DataHandler onData_;
    ExitHandler onExit_;
};

}  // namespace aic
