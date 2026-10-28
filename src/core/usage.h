#pragma once

#include <string>

namespace aic {

// The plan's limits as claude.ai has them now (what the CLI's /usage shows): no turn needed.
struct PlanUsage {
    bool ok = false;
    int sessionPercent = -1;  // the five-hour window
    long long sessionResets = 0;  // Unix seconds
    int weekPercent = -1;
    long long weekResets = 0;
    std::string tier;  // Grok's: "SuperGrok"; its one allowance is in the week fields
    bool monthly = false;  // Grok's allowance runs a month, not a week
};

// Asked with the CLI's own sign-in (~\.claude\.credentials.json); an expired token is left to the CLI to
// renew, then there is no answer until it has. On failure `error` says why.
PlanUsage FetchPlanUsage(std::string& error);

// The answer of api.anthropic.com/api/oauth/usage, for the tests.
PlanUsage ParsePlanUsage(const std::string& json);
// What grok.exe answers to the ACP call _x.ai/billing (what its /usage shows): {"config":{"creditUsagePercent":39.0,
// "currentPeriod":{"type":"USAGE_PERIOD_TYPE_WEEKLY","end":"2026-11-02T00:28:53.388+00:00"}},"subscription_tier":
// "SuperGrok"}. The split into Grok Build, Imagine and chat is only on grok.com's page: the CLI gives the whole.
PlanUsage ParseGrokBilling(const std::string& json);
// "2026-10-21T16:29:59.82+00:00" -> Unix seconds; 0 if it is not one.
long long ParseIsoTime(const std::string& text);

// A moment on Moscow's wall clock: UTC+3 all year (no daylight saving since 2014), whatever zone the PC is set to.
struct MoscowClock {
    int weekday = 0;  // 0 = Sunday
    int hour = 0;
    int minute = 0;
    int day = 1;
    int month = 1;
};
MoscowClock MoscowTime(long long unixSeconds);

// The CLI words a spent limit in English, on a 12-hour clock, in the zone it started in: "You've hit your 5-hour limit ·
// resets 6pm (Europe/Moscow)". Here, in Russian and on the 24-hour Moscow clock: "Лимит сессии (5 ч) исчерпан · сброс в
// 18:00 мск". Any other text, and a time in another zone, comes back as it was.
std::wstring LimitInMoscow(const std::wstring& text);

}  // namespace aic
