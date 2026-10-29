#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aic {

// OAuth for a native app as RFC 8252 has it: the user's own browser (where they are already signed in), the answer
// back on 127.0.0.1 at a port of the moment, PKCE, and a state no other page can guess.

struct HttpsReply {
    int status = 0;  // 0: no answer, `error` says why
    std::string body;
    std::string error;
};

// One HTTPS request through the system's proxy settings; the body is cut at `cap` bytes.
HttpsReply HttpsRequest(const wchar_t* method, const wchar_t* host, const std::wstring& path, const std::wstring& headers,
                        const std::string& body = {}, size_t cap = 4u << 20);

std::string UrlEncode(std::string_view text);
// key=value&… with the values encoded.
std::string FormBody(const std::vector<std::pair<std::string, std::string>>& fields);
inline constexpr const wchar_t* kFormHeaders = L"Content-Type: application/x-www-form-urlencoded\r\n";

// What the browser came back with, for the token request.
struct OAuthGrant {
    std::string code;
    std::string verifier;  // PKCE: the secret whose hash went with the authorization
    std::string redirect;  // the redirect_uri as sent: the token request names it again
};

// The page the browser is left on: AIControls' dark one, the words in its middle.
std::string OAuthPage(std::string_view message, bool ok);

enum class BrowserEnd {
    Answered,   // a code came back and `exchange` ran
    Refused,    // the site said no (the user declined, or it would not have the app)
    Cancelled,
    TimedOut,
    Broken,     // no port to listen on, no random numbers
};

// Opens the browser at `authorize` (its query so far, without redirect_uri, state and the PKCE challenge) and waits
// up to `wait` for the redirect to http://127.0.0.1:<port><path>. With a code, `exchange` runs while the browser waits
// and returns the page to show it (the token is had by then, so the page can say whose it is). Anything but Answered
// sets `error`, in English, `service` naming the site.
BrowserEnd BrowserSignIn(const std::string& authorize, const std::string& path, std::string_view service,
                         const std::function<std::string(const OAuthGrant&)>& exchange, const std::atomic<bool>& cancel,
                         std::chrono::seconds wait, std::string& error);

}  // namespace aic
