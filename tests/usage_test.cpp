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
    std::cout << (failures == 0 ? "all passed\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
