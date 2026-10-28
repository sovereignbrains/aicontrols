#include "core/usage.h"

#include <windows.h>

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <regex>
#include <sstream>

#include <wil/resource.h>

#include "core/claude_process.h"

namespace aic {
namespace {

using unique_hinternet = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

std::string Get(const std::wstring& host, const std::wstring& path, const std::wstring& headers, std::string& error) {
    unique_hinternet session(WinHttpOpen(L"AIControls", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "WinHttpOpen failed";
        return {};
    }
    WinHttpSetTimeouts(session.get(), 10'000, 10'000, 15'000, 15'000);
    unique_hinternet connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    unique_hinternet request(connection ? WinHttpOpenRequest(connection.get(), L"GET", path.c_str(), nullptr,
                                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
    if (!request || !WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA,
                                        0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "no connection";
        return {};
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &size, WINHTTP_NO_HEADER_INDEX);
    std::string body;
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
        body.append(chunk.data(), read);
    }
    if (status != 200) {
        error = "HTTP " + std::to_string(status);
        return {};
    }
    return body;
}

}  // namespace

long long ParseIsoTime(const std::string& text) {
    std::tm time{};
    int seconds = 0;
    if (sscanf_s(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &time.tm_year, &time.tm_mon, &time.tm_mday, &time.tm_hour,
                    &time.tm_min, &seconds) != 6) {
        return 0;
    }
    time.tm_year -= 1900;
    time.tm_mon -= 1;
    time.tm_sec = seconds;
    const long long utc = _mkgmtime(&time);
    // An offset after the time ("+03:00"): back to UTC.
    const size_t sign = text.find_first_of("+-", 19);
    int hours = 0;
    int minutes = 0;
    if (sign != std::string::npos && sscanf_s(text.c_str() + sign + 1, "%2d:%2d", &hours, &minutes) == 2) {
        const long long offset = hours * 3600LL + minutes * 60LL;
        return text[sign] == '+' ? utc - offset : utc + offset;
    }
    return utc < 0 ? 0 : utc;
}

PlanUsage ParseGrokBilling(const std::string& json) {
    PlanUsage usage;
    const Json data = Json::parse(json, nullptr, false);
    const Json& config = JsonAt(data, "config");
    const Json& used = JsonAt(config, "creditUsagePercent");
    if (!used.is_number()) {
        return usage;
    }
    const Json& period = JsonAt(config, "currentPeriod");
    usage.weekPercent = static_cast<int>(std::lround(used.get<double>()));
    usage.weekResets = ParseIsoTime(JsonStr(period, "end").empty() ? JsonStr(config, "billingPeriodEnd")
                                                                   : JsonStr(period, "end"));
    usage.monthly = JsonStr(period, "type").find("MONTH") != std::string::npos;
    usage.tier = JsonStr(data, "subscription_tier");
    usage.ok = true;
    return usage;
}

MoscowClock MoscowTime(long long unixSeconds) {
    const long long moscow = std::max(0LL, unixSeconds) + 3 * 3600;
    const long long days = moscow / 86400;
    const int minutes = static_cast<int>(moscow % 86400 / 60);
    const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{days}}};
    return MoscowClock{static_cast<int>((days + 4) % 7),  // 1 Jan 1970 was a Thursday
                       minutes / 60,
                       minutes % 60,
                       static_cast<int>(static_cast<unsigned>(date.day())),
                       static_cast<int>(static_cast<unsigned>(date.month()))};
}

std::wstring LimitInMoscow(const std::wstring& text) {
    static const std::wregex said(
        LR"(^You['’]ve hit your (.+?) limit(?: · resets (?:(\w{3}) (\d{1,2}), )?(\d{1,2})(?::(\d{2}))?([ap]m)(?: \(([^)]*)\))?)?\.?$)");
    std::wsmatch match;
    if (!std::regex_match(text, match, said)) {
        return text;
    }
    const std::wstring window = match[1];
    std::wstring head;
    if (window == L"5-hour" || window == L"session") {
        head = L"Лимит сессии (5 ч) исчерпан";
    } else if (window == L"weekly" || window == L"week" || window == L"7-day") {
        head = L"Недельный лимит исчерпан";
    } else {
        head = L"Лимит " + window + L" исчерпан";
    }
    if (!match[6].matched) {
        return head;  // no time in it: nothing to move
    }
    if (match[7].matched && match[7].str() != L"Europe/Moscow") {
        return text;  // another zone: not ours to convert
    }
    const int hour = std::stoi(match[4]) % 12 + (match[6] == L"pm" ? 12 : 0);
    const int minute = match[5].matched ? std::stoi(match[5]) : 0;
    std::wstring when = std::format(L"в {:02}:{:02} мск", hour, minute);
    if (match[2].matched) {
        static constexpr const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                                    L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        static constexpr const wchar_t* names[] = {L"янв", L"фев", L"мар", L"апр", L"мая", L"июн",
                                                   L"июл", L"авг", L"сен", L"окт", L"ноя", L"дек"};
        for (size_t i = 0; i < std::size(months); ++i) {
            if (match[2] == months[i]) {
                when = match[3].str() + L" " + names[i] + L" " + when;
                break;
            }
        }
    }
    return head + L" · сброс " + when;
}

PlanUsage ParsePlanUsage(const std::string& json) {
    PlanUsage usage;
    const Json data = Json::parse(json, nullptr, false);
    if (!data.is_object()) {
        return usage;
    }
    const auto window = [](const Json& part, int& percent, long long& resets) {
        const Json& used = JsonAt(part, "utilization");
        if (used.is_number()) {
            percent = static_cast<int>(std::lround(used.get<double>()));
            resets = ParseIsoTime(JsonStr(part, "resets_at"));
        }
    };
    window(JsonAt(data, "five_hour"), usage.sessionPercent, usage.sessionResets);
    window(JsonAt(data, "seven_day"), usage.weekPercent, usage.weekResets);
    usage.ok = usage.sessionPercent >= 0 || usage.weekPercent >= 0;
    return usage;
}

PlanUsage FetchPlanUsage(std::string& error) {
    wchar_t profile[MAX_PATH]{};
    GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    std::ifstream in(std::filesystem::path(profile) / L".claude" / L".credentials.json", std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    const Json credentials = Json::parse(text.str(), nullptr, false);
    const Json& oauth = JsonAt(credentials, "claudeAiOauth");
    const std::string token = JsonStr(oauth, "accessToken");
    if (token.empty()) {
        error = "not signed in";
        return {};
    }
    const Json& expires = JsonAt(oauth, "expiresAt");
    const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    if (expires.is_number() && expires.get<long long>() <= now) {
        error = "the sign-in has expired; the CLI renews it on its next call";
        return {};
    }
    const std::wstring headers = L"Authorization: Bearer " + ToWide(token) + L"\r\nanthropic-beta: oauth-2025-04-20\r\n";
    const std::string body = Get(L"api.anthropic.com", L"/api/oauth/usage", headers, error);
    if (body.empty()) {
        return {};
    }
    PlanUsage usage = ParsePlanUsage(body);
    if (!usage.ok) {
        error = "an answer without limits";
    }
    return usage;
}

}  // namespace aic
