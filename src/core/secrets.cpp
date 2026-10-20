#include "core/secrets.h"

#include <windows.h>

#include <wincrypt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "core/claude_process.h"

namespace aic {
namespace {

// Entropy mixed into DPAPI: another program running as the same user does not decrypt it by accident.
constexpr char kEntropy[] = "AIControls secrets v1";

DATA_BLOB Blob(const std::string& bytes) {
    return DATA_BLOB{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data()))};
}

}  // namespace

std::wstring SecretStore::DefaultPath() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
    std::wstring folder = length > 0 && length < MAX_PATH ? std::wstring(buffer, length) : L".";
    return folder + L"\\AIControls\\secrets.bin";
}

SecretStore::SecretStore(std::wstring path) : path_(std::move(path)) {}

bool SecretStore::Load() {
    std::ifstream file{std::filesystem::path(path_), std::ios::binary};
    if (!file) {
        return false;
    }
    const std::string encrypted((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const std::string entropyText(kEntropy);
    DATA_BLOB in = Blob(encrypted);
    DATA_BLOB entropy = Blob(entropyText);
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    const std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);

    const Json parsed = Json::parse(plain, nullptr, false);
    if (!parsed.is_object()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    values_.clear();
    for (const auto& [name, value] : parsed.items()) {
        if (value.is_string()) {
            values_[name] = value.get<std::string>();
        }
    }
    return true;
}

bool SecretStore::Save() const {
    Json object = Json::object();
    {
        std::lock_guard lock(mutex_);
        for (const auto& [name, value] : values_) {
            object[name] = value;
        }
    }
    std::string plain = object.dump(-1, ' ', false, Json::error_handler_t::replace);
    const std::string entropyText(kEntropy);
    DATA_BLOB in = Blob(plain);
    DATA_BLOB entropy = Blob(entropyText);
    DATA_BLOB out{};
    const BOOL protectedOk =
        CryptProtectData(&in, L"AIControls secrets", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    SecureZeroMemory(plain.data(), plain.size());
    if (!protectedOk) {
        return false;
    }
    const std::filesystem::path target(path_);
    std::error_code ignored;
    std::filesystem::create_directories(target.parent_path(), ignored);
    // Written beside the target and moved over it: a crash never leaves half a file.
    const std::filesystem::path temporary = target.wstring() + L".tmp";
    bool ok = false;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(out.pbData), static_cast<std::streamsize>(out.cbData));
        ok = file.good();
    }
    LocalFree(out.pbData);
    if (!ok) {
        return false;
    }
    return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::vector<std::string> SecretStore::Names() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> names;
    for (const auto& entry : values_) {
        names.push_back(entry.first);
    }
    return names;
}

bool SecretStore::Has(const std::string& name) const {
    std::lock_guard lock(mutex_);
    return values_.contains(name);
}

void SecretStore::Set(const std::string& name, std::string value) {
    std::lock_guard lock(mutex_);
    values_[name] = std::move(value);
}

bool SecretStore::Remove(const std::string& name) {
    std::lock_guard lock(mutex_);
    return values_.erase(name) > 0;
}

std::string SecretStore::Substitute(std::string_view text, std::vector<std::string>& missing) const {
    constexpr std::string_view open = "{{secret:";
    std::string out;
    std::lock_guard lock(mutex_);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t start = text.find(open, pos);
        const size_t end = start == std::string_view::npos ? start : text.find("}}", start + open.size());
        if (end == std::string_view::npos) {
            out.append(text.substr(pos));
            break;
        }
        out.append(text.substr(pos, start - pos));
        const std::string name(text.substr(start + open.size(), end - start - open.size()));
        if (const auto it = values_.find(name); it != values_.end()) {
            out += it->second;
        } else {
            missing.push_back(name);
            out.append(text.substr(start, end + 2 - start));
        }
        pos = end + 2;
    }
    return out;
}

std::string SecretStore::Redact(std::string_view text) const {
    std::vector<std::string> values;
    {
        std::lock_guard lock(mutex_);
        for (const auto& entry : values_) {
            if (entry.second.size() >= 3) {  // shorter ones would shred ordinary output
                values.push_back(entry.second);
            }
        }
    }
    // Longest first, so a value that contains another is cut out whole.
    std::ranges::sort(values, [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::string out(text);
    for (const std::string& value : values) {
        for (size_t at = out.find(value); at != std::string::npos; at = out.find(value, at + 3)) {
            out.replace(at, value.size(), "***");
        }
    }
    return out;
}

std::vector<std::pair<std::wstring, std::wstring>> SecretStore::Environment(
    const std::vector<std::string>& names, std::vector<std::string>& missing) const {
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    std::lock_guard lock(mutex_);
    for (const std::string& name : names) {
        if (const auto it = values_.find(name); it != values_.end()) {
            pairs.emplace_back(ToWide(name), ToWide(it->second));
        } else {
            missing.push_back(name);
        }
    }
    return pairs;
}

}  // namespace aic
