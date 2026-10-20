#include "core/pty.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace aic {
namespace {

[[noreturn]] void ThrowLastError(const char* what) {
    const DWORD error = GetLastError();
    throw std::runtime_error(std::string(what) + " failed, error " + std::to_string(error));
}

COORD Size(int cols, int rows) {
    return COORD{static_cast<SHORT>(std::clamp(cols, 1, 1000)), static_cast<SHORT>(std::clamp(rows, 1, 1000))};
}

}  // namespace

Pty::~Pty() {
    Stop();
}

void Pty::Start(int cols, int rows, const std::wstring& commandLine, const std::wstring& cwd, DataHandler onData,
                ExitHandler onExit) {
    Stop();
    stopping_ = false;
    onData_ = std::move(onData);
    onExit_ = std::move(onExit);

    wil::unique_handle inRead, inWrite, outRead, outWrite;
    if (!CreatePipe(inRead.put(), inWrite.put(), nullptr, 0) || !CreatePipe(outRead.put(), outWrite.put(), nullptr, 0)) {
        ThrowLastError("CreatePipe");
    }
    HPCON console = nullptr;
    if (const HRESULT hr = CreatePseudoConsole(Size(cols, rows), inRead.get(), outWrite.get(), 0, &console);
        FAILED(hr)) {
        throw std::runtime_error("CreatePseudoConsole failed, HRESULT " + std::to_string(static_cast<long>(hr)));
    }
    auto closeConsole = wil::scope_exit([&] {
        if (console != nullptr) {
            ClosePseudoConsole(console);
        }
    });

    SIZE_T attributesSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributesSize);
    std::vector<std::byte> attributesBuffer(attributesSize);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributesBuffer.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributesSize)) {
        ThrowLastError("InitializeProcThreadAttributeList");
    }
    auto deleteAttributes = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(console), nullptr,
                                   nullptr)) {
        ThrowLastError("UpdateProcThreadAttribute");
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Empty standard handles: otherwise a parent whose own stdio is redirected passes it on, and the program
    // talks to that instead of the pseudo console.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList = attributes;

    wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        ThrowLastError("CreateJobObject");
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    std::wstring command = commandLine;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                        &startup.StartupInfo, &info)) {
        ThrowLastError("CreateProcess");
    }
    wil::unique_handle process(info.hProcess);
    const wil::unique_handle thread(info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        ThrowLastError("AssignProcessToJobObject");
    }
    ResumeThread(thread.get());

    // The pseudo console holds its own copies of these two ends.
    inRead.reset();
    outWrite.reset();

    closeConsole.release();
    pseudoConsole_ = console;
    process_ = std::move(process);
    job_ = std::move(job);
    input_ = std::move(inWrite);
    output_ = std::move(outRead);
    reader_ = std::thread(&Pty::ReadLoop, this);
    waiter_ = std::thread(&Pty::WaitLoop, this);
}

void Pty::Stop() {
    stopping_ = true;
    if (pseudoConsole_ != nullptr) {
        ClosePseudoConsole(pseudoConsole_);  // the reader keeps draining, so this cannot block on a full pipe
        pseudoConsole_ = nullptr;
    }
    if (job_) {
        TerminateJobObject(job_.get(), 1);
    }
    {
        std::lock_guard lock(writeMutex_);
        input_.reset();
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (waiter_.joinable()) {
        waiter_.join();
    }
    job_.reset();
    process_.reset();
    output_.reset();
}

void Pty::Write(std::string_view bytes) {
    std::lock_guard lock(writeMutex_);
    if (!input_) {
        return;
    }
    const char* data = bytes.data();
    size_t left = bytes.size();
    while (left > 0) {
        DWORD written = 0;
        if (!WriteFile(input_.get(), data, static_cast<DWORD>(std::min<size_t>(left, 1 << 16)), &written, nullptr)) {
            return;
        }
        data += written;
        left -= written;
    }
}

void Pty::Resize(int cols, int rows) {
    if (pseudoConsole_ != nullptr) {
        ResizePseudoConsole(pseudoConsole_, Size(cols, rows));
    }
}

void Pty::ReadLoop() {
    std::vector<char> chunk(16384);
    DWORD got = 0;
    while (::ReadFile(output_.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr) && got > 0) {
        if (!stopping_ && onData_) {
            onData_(std::string(chunk.data(), got));
        }
    }
}

// The pseudo console keeps its output open after the program ends, so the exit is watched for apart from the
// reading, which ends only when Stop() closes the console.
void Pty::WaitLoop() {
    while (!stopping_) {
        if (WaitForSingleObject(process_.get(), 100) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(process_.get(), &code);
            Sleep(150);  // the last output is still on its way through the pipe
            if (!stopping_ && onExit_) {
                onExit_(code);
            }
            return;
        }
    }
}

}  // namespace aic
