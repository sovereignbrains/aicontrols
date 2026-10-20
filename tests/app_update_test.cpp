// Finding a newer AIControls installer. Exit code 0: all passed.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "core/app_update.h"

namespace {

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    Expect(aic::SetupVersion(L"AIControls-Setup-0.1.3.exe") == "0.1.3", "the version from the name");
    Expect(aic::SetupVersion(L"AIControls-Setup-.exe").empty() && aic::SetupVersion(L"AIControls-Setup-0.1.x.exe").empty() &&
               aic::SetupVersion(L"Other-0.1.3.exe").empty() && aic::SetupVersion(L"AIControls-Setup-0.1.3.exe.part").empty(),
           "other names have none");

    const fs::path dir = fs::temp_directory_path() / L"aic_app_update_test";
    std::error_code error;
    fs::remove_all(dir, error);
    fs::create_directories(dir, error);
    for (const wchar_t* name : {L"AIControls-Setup-0.1.2.exe", L"AIControls-Setup-0.1.10.exe", L"AIControls-Setup-0.1.9.exe",
                                L"notes.txt"}) {
        std::ofstream(dir / name) << "x";
    }
    const auto update = aic::FindAppUpdate({dir.wstring()}, "0.1.3");
    Expect(update && update->version == "0.1.10" && fs::path(update->installer).filename() == L"AIControls-Setup-0.1.10.exe",
           "the newest, compared as numbers");
    Expect(!aic::FindAppUpdate({dir.wstring()}, "0.1.10"), "nothing newer than what is installed");
    Expect(aic::FindAppUpdate({dir.wstring()}, "") && !aic::FindAppUpdate({(dir / L"missing").wstring()}, "0.1.0"),
           "never installed: any; a missing folder: none");
    fs::remove_all(dir, error);
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
