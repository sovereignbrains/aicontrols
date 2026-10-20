#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aic {

// A newer AIControls to install over this one.
struct AppUpdate {
    std::string version;    // 0.1.3
    std::wstring installer; // AIControls-Setup-0.1.3.exe, full path
};

// The version the installer recorded (its uninstall entry); empty when AIControls was never installed.
std::string InstalledAppVersion();

// "AIControls-Setup-0.1.3.exe" -> "0.1.3"; empty for any other name.
std::string SetupVersion(const std::wstring& fileName);

// The newest installer in these folders, if it is newer than `installed` (an empty `installed`: any is).
std::optional<AppUpdate> FindAppUpdate(const std::vector<std::wstring>& folders, const std::string& installed);

// Where builds of AIControls land: %USERPROFILE%\projects\aicontrols\build\installer.
std::vector<std::wstring> AppUpdateFolders();

}  // namespace aic
