#pragma once

#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aic {

// "name → value" secrets in one file, encrypted with DPAPI (bound to the Windows account, no password).
// Values never leave through the chat: commands name them ({{secret:NAME}}, env_secrets) and whatever a
// command prints is scrubbed of every value. Thread-safe: tools run on worker threads.
class SecretStore {
public:
    // %APPDATA%\AIControls\secrets.bin
    static std::wstring DefaultPath();

    explicit SecretStore(std::wstring path);

    // false when the file is missing or cannot be decrypted (the store stays empty).
    bool Load();
    bool Save() const;

    std::vector<std::string> Names() const;
    bool Has(const std::string& name) const;
    void Set(const std::string& name, std::string value);
    bool Remove(const std::string& name);

    // Replaces every {{secret:NAME}}; names that are not stored are appended to `missing`.
    std::string Substitute(std::string_view text, std::vector<std::string>& missing) const;
    // Every stored value in `text` becomes ***.
    std::string Redact(std::string_view text) const;
    // NAME=value pairs for the child's environment; unknown names go to `missing`.
    std::vector<std::pair<std::wstring, std::wstring>> Environment(const std::vector<std::string>& names,
                                                                   std::vector<std::string>& missing) const;

private:
    std::wstring path_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> values_;  // guarded by mutex_
};

}  // namespace aic
