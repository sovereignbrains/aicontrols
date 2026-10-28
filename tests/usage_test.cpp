// Reading the plan's limits from claude.ai's answer. Exit code 0: all passed.
#include <iostream>

#include "core/usage.h"

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
    Expect(aic::ParseIsoTime("1970-01-01T00:01:00+00:00") == 60, "UTC");
    Expect(aic::ParseIsoTime("1970-01-01T03:01:00.5+03:00") == 60, "an offset, and fractions of a second");
    Expect(aic::ParseIsoTime("2026-10-21T16:29:59.821995+00:00") == 1792600199, "a real one");
    Expect(aic::ParseIsoTime("soon") == 0, "not a time");

    const auto usage = aic::ParsePlanUsage(
        R"({"five_hour":{"utilization":10.4,"resets_at":"1970-01-01T00:01:00+00:00"},)"
        R"("seven_day":{"utilization":36.0,"resets_at":"1970-01-01T00:02:00+00:00"},"seven_day_opus":null})");
    Expect(usage.ok && usage.sessionPercent == 10 && usage.sessionResets == 60 && usage.weekPercent == 36 &&
               usage.weekResets == 120,
           "both windows, in percent");
    Expect(!aic::ParsePlanUsage(R"({"error":"x"})").ok && !aic::ParsePlanUsage("<html>").ok, "no limits, not ok");

    const auto grok = aic::ParseGrokBilling(
        R"({"config":{"creditUsagePercent":39.0,"currentPeriod":{"type":"USAGE_PERIOD_TYPE_WEEKLY","start":"2026-10-26T00:28:53.388+00:00",)"
        R"("end":"2026-11-02T00:28:53.388+00:00"},"prepaidBalance":{"val":0},"isUnifiedBillingUser":true},"subscription_tier":"SuperGrok"})");
    Expect(grok.ok && grok.weekPercent == 39 && grok.weekResets == 1793579333 && grok.tier == "SuperGrok" && !grok.monthly,
           "Grok: the week's share, when it ends, the tier");
    Expect(aic::ParseGrokBilling(R"({"config":{"currentPeriod":{"type":"USAGE_PERIOD_TYPE_MONTHLY"}}})").ok == false &&
               aic::ParseGrokBilling("nope").ok == false,
           "Grok: no share, not ok");
    Expect(aic::ParseGrokBilling(R"({"config":{"creditUsagePercent":4,"currentPeriod":{"type":"USAGE_PERIOD_TYPE_MONTHLY"}}})")
                   .monthly,
           "Grok: a month");

    const auto epoch = aic::MoscowTime(0);
    Expect(epoch.weekday == 4 && epoch.hour == 3 && epoch.minute == 0, "Moscow: the epoch was a Thursday, 03:00");
    const auto real = aic::MoscowTime(1792600199);  // 2026-10-21T16:29:59Z
    Expect(real.weekday == 3 && real.hour == 19 && real.minute == 29, "Moscow: UTC+3, the day of the week");
    const auto midnight = aic::MoscowTime(21 * 3600);  // 21:00 UTC is already the next day there
    Expect(midnight.weekday == 5 && midnight.hour == 0, "Moscow: past midnight, the next day");
    const auto reset = aic::MoscowTime(1793579333);  // Grok's week ends 2026-11-02T00:28Z
    Expect(reset.weekday == 1 && reset.day == 2 && reset.month == 11 && reset.hour == 3 && reset.minute == 28,
           "Moscow: Monday 2 November, 03:28");

    Expect(aic::LimitInMoscow(L"You've hit your 5-hour limit · resets 6pm (Europe/Moscow)") ==
               L"Лимит сессии (5 ч) исчерпан · сброс в 18:00 мск",
           "limit: 6pm is 18:00");
    Expect(aic::LimitInMoscow(L"You’ve hit your session limit · resets 12:30am (Europe/Moscow)") ==
               L"Лимит сессии (5 ч) исчерпан · сброс в 00:30 мск",
           "limit: 12am is 00, a curly apostrophe, minutes");
    Expect(aic::LimitInMoscow(L"You've hit your weekly limit · resets Oct 29, 3am (Europe/Moscow)") ==
               L"Недельный лимит исчерпан · сброс 29 окт в 03:00 мск",
           "limit: a date, in Russian");
    Expect(aic::LimitInMoscow(L"You've hit your Opus limit · resets 12pm (Europe/Moscow)") ==
               L"Лимит Opus исчерпан · сброс в 12:00 мск",
           "limit: 12pm is noon");
    Expect(aic::LimitInMoscow(L"You've hit your 5-hour limit · resets 6pm (America/New_York)") ==
               L"You've hit your 5-hour limit · resets 6pm (America/New_York)",
           "limit: another zone stays as it was");
    Expect(aic::LimitInMoscow(L"Всё хорошо, сброс в 6pm") == L"Всё хорошо, сброс в 6pm", "other text stays");
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
