// Pictures kept for the chat: saved once by their content, read back as previews. Exit code 0: all passed.
#include <windows.h>
#include <wincodec.h>

#include <filesystem>
#include <iostream>
#include <string>

#include <wil/com.h>

#include "core/attachment.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

// A 1x1 PNG.
constexpr const char* kPixel =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8DwHwAFBQIAX8jx0gAAAABJRU5ErkJggg==";

}  // namespace

int main() {
    namespace fs = std::filesystem;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return 1;
    }
    // The pictures go under %LOCALAPPDATA%: a scratch one here, not the user's.
    const fs::path scratch = fs::temp_directory_path() / L"aic_attachment_test";
    std::error_code error;
    fs::remove_all(scratch, error);
    fs::create_directories(scratch, error);
    SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str());
    {
        const std::wstring first = aic::SaveChatImage(kPixel, "image/png");
        const std::wstring again = aic::SaveChatImage(kPixel, "image/png");
        Expect(!first.empty() && fs::exists(first), "a picture is saved");
        Expect(first == again, "the same picture is the same file");
        Expect(fs::path(first).extension() == L".png" && fs::path(first).parent_path() == scratch / L"AIControls" / L"images",
               "saved where it belongs, by its type");
        Expect(aic::SaveChatImage("", "image/png").empty(), "nothing to save, no file");

        wil::com_ptr<IWICImagingFactory> wic = wil::CoCreateInstance<IWICImagingFactory>(CLSID_WICImagingFactory);
        UINT width = 0;
        UINT height = 0;
        Expect(aic::LoadImagePreview(wic.get(), first, 64, width, height) && width == 1 && height == 1,
               "the preview reads it back with its size");
        Expect(!aic::LoadImagePreview(wic.get(), (scratch / L"missing.png").wstring(), 64, width, height) && width == 0,
               "a missing file gives no preview");
    }
    fs::remove_all(scratch, error);
    CoUninitialize();
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
