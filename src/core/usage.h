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
};

// Asked with the CLI's own sign-in (~\.claude\.credentials.json); an expired token is left to the CLI to
// renew, then there is no answer until it has. On failure `error` says why.
PlanUsage FetchPlanUsage(std::string& error);

// The answer of api.anthropic.com/api/oauth/usage, for the tests.
PlanUsage ParsePlanUsage(const std::string& json);
// "2026-10-21T16:29:59.82+00:00" -> Unix seconds; 0 if it is not one.
long long ParseIsoTime(const std::string& text);

}  // namespace aic
