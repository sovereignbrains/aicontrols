#pragma once

#include <atomic>
#include <string>
#include <string_view>

#include "core/tools.h"

namespace aic {

// Google Drive for the agents (read only: the drive.readonly scope). The OAuth client is the user's own, in the
// secrets GDRIVE_CLIENT_ID and GDRIVE_CLIENT_SECRET; the first call opens the browser for the consent and keeps
// the refresh token in GDRIVE_REFRESH_TOKEN (the same DPAPI store, never shown to the model).
class GoogleDrive {
public:
    explicit GoogleDrive(SecretStore& secrets) : secrets_(secrets) {}

    // drive_search: text (name and content), folder (an id), q (Drive's own query syntax), limit.
    ToolResult Search(const Json& input, const std::atomic<bool>& cancel) const;
    // drive_read: id, offset and limit in bytes. Docs, Sheets (the first sheet) and Slides come as text.
    ToolResult Read(const Json& input, const std::atomic<bool>& cancel) const;

    // The `q` of files.list for a drive_search call; trashed files are always left out.
    static std::string Query(const Json& input);
    // The text type a Google file is exported as; empty when it has none (a folder, a drawing, a form).
    static std::string ExportType(std::string_view driveType);
    // Whether a stored file is worth reading as text.
    static bool TextFile(std::string_view mimeType, std::string_view name);
    static std::string UrlEncode(std::string_view text);

private:
    // A fresh access token; signs in through the browser when there is no refresh token. Empty with `error` set.
    std::string AccessToken(const std::atomic<bool>& cancel, std::string& error) const;
    std::string SignIn(const std::string& clientId, const std::string& clientSecret, const std::atomic<bool>& cancel,
                       std::string& error) const;

    SecretStore& secrets_;
};

}  // namespace aic
