#pragma once

#include <windows.h>
#include <mmsystem.h>  // not in WIN32_LEAN_AND_MEAN

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aic {

// The default microphone, 16 kHz mono 16-bit (what speech recognition wants), recorded until Stop.
class Recorder {
public:
    Recorder() = default;
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // False with `error` set when there is no microphone (or it is refused).
    bool Start(std::string& error);
    // What was heard, as a WAV file; empty when nothing was.
    std::vector<uint8_t> Stop();
    bool Recording() const { return thread_.joinable(); }
    float Seconds() const;  // recorded so far

private:
    void Loop();

    HWAVEIN device_ = nullptr;
    HANDLE ready_ = nullptr;  // a buffer has been filled
    std::thread thread_;
    std::atomic<bool> stop_ = false;
    mutable std::mutex mutex_;
    std::vector<uint8_t> pcm_;  // guarded by mutex_
};

// The PCM wrapped as a WAV file.
std::vector<uint8_t> MakeWav(const std::vector<uint8_t>& pcm, unsigned rate, unsigned channels, unsigned bits);

// Speech to text with Groq's Whisper (whisper-large-v3-turbo); the key is the GROC secret. Empty on failure,
// with `error` set.
std::string Transcribe(const std::vector<uint8_t>& wav, const std::string& apiKey, std::string& error);

}  // namespace aic
