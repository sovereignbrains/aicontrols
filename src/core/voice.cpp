#include "core/voice.h"

#include <mmsystem.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include <wil/resource.h>

#include "core/claude_process.h"

namespace aic {
namespace {

constexpr unsigned kRate = 16000;
constexpr unsigned kChannels = 1;
constexpr unsigned kBits = 16;
constexpr size_t kBuffer = kRate * kChannels * kBits / 8 / 4;  // a quarter of a second
constexpr size_t kBuffers = 4;
constexpr size_t kLongest = kRate * kChannels * kBits / 8 * 600;  // ten minutes: more is not kept

using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void Put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void PutText(std::vector<uint8_t>& out, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
}

}  // namespace

Recorder::~Recorder() {
    Stop();
}

bool Recorder::Start(std::string& error) {
    if (Recording()) {
        return true;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kChannels;
    format.nSamplesPerSec = kRate;
    format.wBitsPerSample = kBits;
    format.nBlockAlign = static_cast<WORD>(kChannels * kBits / 8);
    format.nAvgBytesPerSec = kRate * format.nBlockAlign;
    ready_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ready_ == nullptr) {
        error = "no event";
        return false;
    }
    const MMRESULT opened = waveInOpen(&device_, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(ready_), 0,
                                       CALLBACK_EVENT);
    if (opened != MMSYSERR_NOERROR) {
        CloseHandle(ready_);
        ready_ = nullptr;
        device_ = nullptr;
        error = opened == WAVERR_BADFORMAT ? "the microphone does not record 16 kHz" : "no microphone";
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        pcm_.clear();
    }
    stop_ = false;
    thread_ = std::thread([this] { Loop(); });
    return true;
}

void Recorder::Loop() {
    std::array<std::vector<char>, kBuffers> data;
    std::array<WAVEHDR, kBuffers> headers{};
    for (size_t i = 0; i < kBuffers; ++i) {
        data[i].resize(kBuffer);
        headers[i].lpData = data[i].data();
        headers[i].dwBufferLength = static_cast<DWORD>(kBuffer);
        waveInPrepareHeader(device_, &headers[i], sizeof(WAVEHDR));
        waveInAddBuffer(device_, &headers[i], sizeof(WAVEHDR));
    }
    waveInStart(device_);
    const auto collect = [&](bool requeue) {
        for (WAVEHDR& header : headers) {
            if ((header.dwFlags & WHDR_DONE) == 0) {
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                if (pcm_.size() + header.dwBytesRecorded <= kLongest) {
                    pcm_.insert(pcm_.end(), header.lpData, header.lpData + header.dwBytesRecorded);
                }
            }
            header.dwFlags &= ~static_cast<DWORD>(WHDR_DONE);
            header.dwBytesRecorded = 0;
            if (requeue) {
                waveInAddBuffer(device_, &header, sizeof(WAVEHDR));
            }
        }
    };
    while (!stop_) {
        WaitForSingleObject(ready_, 100);
        collect(true);
    }
    waveInStop(device_);
    waveInReset(device_);  // the partly filled buffers come back marked done
    collect(false);
    for (WAVEHDR& header : headers) {
        waveInUnprepareHeader(device_, &header, sizeof(WAVEHDR));
    }
}

std::vector<uint8_t> Recorder::Stop() {
    if (!thread_.joinable()) {
        return {};
    }
    stop_ = true;
    thread_.join();
    waveInClose(device_);
    device_ = nullptr;
    CloseHandle(ready_);
    ready_ = nullptr;
    std::lock_guard lock(mutex_);
    if (pcm_.size() < kRate * kBits / 8 / 4) {  // under a quarter of a second: nothing said
        return {};
    }
    // Silence is not sent: Whisper makes words up for it ("Thank you.").
    int peak = 0;
    for (size_t i = 0; i + 1 < pcm_.size(); i += 2) {
        const auto sample = static_cast<int16_t>(pcm_[i] | (pcm_[i + 1] << 8));
        peak = std::max(peak, std::abs(static_cast<int>(sample)));
    }
    if (peak < 600) {
        return {};
    }
    return MakeWav(pcm_, kRate, kChannels, kBits);
}

float Recorder::Seconds() const {
    std::lock_guard lock(mutex_);
    return static_cast<float>(pcm_.size()) / static_cast<float>(kRate * kChannels * kBits / 8);
}

std::vector<uint8_t> MakeWav(const std::vector<uint8_t>& pcm, unsigned rate, unsigned channels, unsigned bits) {
    std::vector<uint8_t> out;
    out.reserve(44 + pcm.size());
    PutText(out, "RIFF");
    Put32(out, static_cast<uint32_t>(36 + pcm.size()));
    PutText(out, "WAVEfmt ");
    Put32(out, 16);
    Put16(out, 1);  // PCM
    Put16(out, static_cast<uint16_t>(channels));
    Put32(out, rate);
    Put32(out, rate * channels * bits / 8);
    Put16(out, static_cast<uint16_t>(channels * bits / 8));
    Put16(out, static_cast<uint16_t>(bits));
    PutText(out, "data");
    Put32(out, static_cast<uint32_t>(pcm.size()));
    out.insert(out.end(), pcm.begin(), pcm.end());
    return out;
}

std::string Transcribe(const std::vector<uint8_t>& wav, const std::string& apiKey, std::string& error) {
    if (wav.empty()) {
        error = "nothing recorded";
        return {};
    }
    const std::string boundary = "----aicontrols-voice-7f3a";
    std::vector<uint8_t> body;
    const auto field = [&](const std::string& name, const std::string& value) {
        PutText(body, "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n");
    };
    field("model", "whisper-large-v3-turbo");
    field("response_format", "json");
    field("temperature", "0");
    field("language", "ru");  // the user speaks Russian; English terms in it still come out right
    PutText(body, "--" + boundary +
                      "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"voice.wav\"\r\n"
                      "Content-Type: audio/wav\r\n\r\n");
    body.insert(body.end(), wav.begin(), wav.end());
    PutText(body, "\r\n--" + boundary + "--\r\n");

    unique_hinternet session(WinHttpOpen(L"AIControls", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "WinHttpOpen failed";
        return {};
    }
    WinHttpSetTimeouts(session.get(), 10'000, 15'000, 60'000, 120'000);
    unique_hinternet connection(WinHttpConnect(session.get(), L"api.groq.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    unique_hinternet request(connection ? WinHttpOpenRequest(connection.get(), L"POST", L"/openai/v1/audio/transcriptions",
                                                             nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
    const std::wstring headers = L"Authorization: Bearer " + ToWide(apiKey) +
                                 L"\r\nContent-Type: multipart/form-data; boundary=" + ToWide(boundary) + L"\r\n";
    if (!request || !WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L), body.data(),
                                        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "no connection to api.groq.com";
        return {};
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string answer;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &read) || read == 0) {
            break;
        }
        answer.append(chunk.data(), read);
    }
    const Json data = Json::parse(answer, nullptr, false);
    if (status != 200) {
        const std::string message = JsonStr(JsonAt(data, "error"), "message");
        error = "HTTP " + std::to_string(status) + (message.empty() ? "" : ": " + message);
        return {};
    }
    std::string text = JsonStr(data, "text");
    const size_t first = text.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

}  // namespace aic
