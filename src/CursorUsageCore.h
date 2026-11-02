#pragma once

#include <optional>
#include <string>

struct UsageWindow {
    int usedPercent = -1;
    int remainingPercent = -1;
    int resetAfterSeconds = 0;
    long long resetAtUnixSeconds = 0;
};

struct UsageSnapshot {
    bool success = false;
    std::wstring email;
    std::wstring planType;
    std::wstring displayMessage;
    std::wstring errorMessage;
    UsageWindow cursorModels;
    UsageWindow otherModels;
    int remainingCents = -1;
    int limitCents = -1;
};

std::wstring Utf8ToWide(const std::string& input);
std::string WideToUtf8(const std::wstring& input);
UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage);
std::wstring FormatRemainingPercent(int remainingPercent);
std::wstring FormatResetAfter(int seconds);
std::wstring FormatUserDisplay(const UsageSnapshot& snapshot);
std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot);
