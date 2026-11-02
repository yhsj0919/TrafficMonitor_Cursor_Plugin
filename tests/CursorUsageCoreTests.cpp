#include "CursorUsageCore.h"
#include "CursorUsageVersion.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void AssertTrue(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void AssertEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        std::cerr << message << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void AssertEqual(const std::wstring& actual, const std::wstring& expected, const char* message) {
    if (actual != expected) {
        std::wcerr << message << L": expected [" << expected << L"], got [" << actual << L"]\n";
        std::exit(1);
    }
}

void ParsesUsageWindows() {
    const std::string json = R"json({
        "billingCycleStart": "1768399334000",
        "billingCycleEnd": "1771077734000",
        "planUsage": {
            "totalSpend": 23222,
            "includedSpend": 23222,
            "remaining": 16778,
            "limit": 40000,
            "autoPercentUsed": 25.2,
            "apiPercentUsed": 50,
            "totalPercentUsed": 15.48
        },
        "displayMessage": "You've used 50% of your usage limit"
    })json";

    std::wstring error;
    UsageSnapshot snapshot = ParseUsageJson(json, &error);

    AssertTrue(snapshot.success, "snapshot should parse");
    AssertEqual(snapshot.cursorModels.usedPercent, 25, "cursor models used percent");
    AssertEqual(snapshot.cursorModels.remainingPercent, 75, "cursor models remaining percent");
    AssertEqual(snapshot.otherModels.usedPercent, 50, "other models used percent");
    AssertEqual(snapshot.otherModels.remainingPercent, 50, "other models remaining percent");
    AssertEqual(snapshot.remainingCents, 16778, "remaining cents");
    AssertEqual(snapshot.limitCents, 40000, "limit cents");
}

void AllowsSinglePool() {
    const std::string json = R"json({
        "billingCycleEnd": "1771077734000",
        "planUsage": {
            "apiPercentUsed": 10
        }
    })json";

    std::wstring error;
    UsageSnapshot snapshot = ParseUsageJson(json, &error);

    AssertTrue(snapshot.success, "snapshot should parse with only api pool");
    AssertTrue(error.empty(), "missing auto pool should not set an error");
    AssertEqual(snapshot.cursorModels.remainingPercent, -1, "missing cursor models should be unavailable");
    AssertEqual(snapshot.otherModels.remainingPercent, 90, "other models remaining percent");
}

void FormatsDisplayText() {
    AssertEqual(FormatRemainingPercent(75), L"75%", "remaining percent text");
    AssertEqual(FormatRemainingPercent(-1), L"--", "negative percent text");
    AssertEqual(FormatRemainingPercent(101), L"--", "out-of-range percent text");
}

void FormatsUserDisplay() {
    UsageSnapshot snapshot;
    AssertEqual(FormatUserDisplay(snapshot), L"--", "empty user display");
    snapshot.email = L"user@example.com";
    AssertEqual(FormatUserDisplay(snapshot), L"user@example.com", "email user display");
}

void BuildsMultiLineSuccessTooltip() {
    UsageSnapshot snapshot;
    snapshot.success = true;
    snapshot.email = L"user@example.com";
    snapshot.cursorModels.usedPercent = 25;
    snapshot.cursorModels.remainingPercent = 75;
    snapshot.cursorModels.resetAfterSeconds = 3600;
    snapshot.otherModels.usedPercent = 50;
    snapshot.otherModels.remainingPercent = 50;
    snapshot.otherModels.resetAfterSeconds = 86400;
    snapshot.planType = L"ultra";

    const std::wstring tooltip = BuildUsageTooltip(snapshot);

    AssertEqual(
        tooltip,
        L"\u7528\u6237: user@example.com\n"
        L"Cursor Models \u5269\u4f59 75% (\u5df2\u7528 25%\uff0c1h 0m\u540e\u91cd\u7f6e)\n"
        L"Other Models \u5269\u4f59 50% (\u5df2\u7528 50%\uff0c1d 0h\u540e\u91cd\u7f6e)\n"
        L"\u5957\u9910: ultra",
        "multi-line success tooltip");
}

void HasGeneratedVersion() {
    AssertTrue(std::wstring(CURSOR_USAGE_VERSION_WIDE).size() > 0, "generated version should not be empty");
}

}  // namespace

void RunCursorConfigTests();

int main() {
    RunCursorConfigTests();
    ParsesUsageWindows();
    AllowsSinglePool();
    FormatsDisplayText();
    FormatsUserDisplay();
    BuildsMultiLineSuccessTooltip();
    HasGeneratedVersion();
    return 0;
}
