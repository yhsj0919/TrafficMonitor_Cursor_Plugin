#include "CursorUsageCore.h"

#include "JsonLite.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <ctime>

namespace {

bool ExtractPercentWindow(const jsonlite::Value* usedNode, long long resetAtUnixSeconds, int resetAfterSeconds,
                          UsageWindow* output) {
    if (usedNode == nullptr || output == nullptr) {
        return false;
    }

    auto usedNumber = usedNode->AsNumber();
    if (!usedNumber.has_value()) {
        return false;
    }

    const int usedPercent = static_cast<int>(std::lround(std::clamp(*usedNumber, 0.0, 100.0)));
    output->usedPercent = usedPercent;
    output->remainingPercent = 100 - usedPercent;
    output->resetAtUnixSeconds = resetAtUnixSeconds;
    output->resetAfterSeconds = std::max(resetAfterSeconds, 0);
    return true;
}

long long ParseUnixMs(const jsonlite::Value* node) {
    if (node == nullptr) {
        return 0;
    }
    if (auto number = node->AsNumber(); number.has_value()) {
        return static_cast<long long>(*number);
    }
    if (auto text = node->AsString(); text.has_value() && !text->empty()) {
        try {
            return std::stoll(std::string(*text));
        } catch (...) {
            return 0;
        }
    }
    return 0;
}

int SecondsUntilUnixMs(long long unixMs) {
    if (unixMs <= 0) {
        return 0;
    }
    const long long nowMs = static_cast<long long>(std::time(nullptr)) * 1000;
    const long long deltaMs = unixMs - nowMs;
    if (deltaMs <= 0) {
        return 0;
    }
    return static_cast<int>(deltaMs / 1000);
}

}  // namespace

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring output(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size);
    return output;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string output(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr);
    return output;
}

UsageSnapshot ParseUsageJson(const std::string& jsonText, std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    UsageSnapshot snapshot;
    jsonlite::Parser parser(jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage JSON parse failed: " + Utf8ToWide(parser.Error());
        }
        return snapshot;
    }

    const jsonlite::Value* planUsage = root->Find("planUsage");
    if (planUsage == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage payload missing planUsage";
        }
        return snapshot;
    }

    const long long billingCycleEndMs = ParseUnixMs(root->Find("billingCycleEnd"));
    const int resetAfterSeconds = SecondsUntilUnixMs(billingCycleEndMs);
    const long long resetAtUnixSeconds = billingCycleEndMs > 0 ? billingCycleEndMs / 1000 : 0;

    const jsonlite::Value* autoPercent = planUsage->Find("autoPercentUsed");
    const jsonlite::Value* apiPercent = planUsage->Find("apiPercentUsed");
    const bool hasAuto = ExtractPercentWindow(autoPercent, resetAtUnixSeconds, resetAfterSeconds, &snapshot.cursorModels);
    const bool hasApi = ExtractPercentWindow(apiPercent, resetAtUnixSeconds, resetAfterSeconds, &snapshot.otherModels);
    if (!hasAuto && !hasApi) {
        if (errorMessage != nullptr) {
            *errorMessage = L"usage payload missing autoPercentUsed/apiPercentUsed";
        }
        return snapshot;
    }

    if (const jsonlite::Value* remaining = planUsage->Find("remaining"); remaining != nullptr) {
        if (auto cents = remaining->AsInt(); cents.has_value()) {
            snapshot.remainingCents = std::max(*cents, 0);
        }
    }
    if (const jsonlite::Value* limit = planUsage->Find("limit"); limit != nullptr) {
        if (auto cents = limit->AsInt(); cents.has_value()) {
            snapshot.limitCents = std::max(*cents, 0);
        }
    }

    if (const jsonlite::Value* displayMessage = root->Find("displayMessage"); displayMessage != nullptr) {
        if (auto text = displayMessage->AsString(); text.has_value()) {
            snapshot.displayMessage = Utf8ToWide(std::string(*text));
        }
    }

    snapshot.success = true;
    return snapshot;
}

std::wstring FormatRemainingPercent(int remainingPercent) {
    if (remainingPercent < 0 || remainingPercent > 100) {
        return L"--";
    }
    return std::to_wstring(remainingPercent) + L"%";
}

std::wstring FormatResetAfter(int seconds) {
    if (seconds <= 0) {
        return L"now";
    }

    const int days = seconds / 86400;
    seconds %= 86400;
    const int hours = seconds / 3600;
    seconds %= 3600;
    const int minutes = seconds / 60;

    if (days > 0) {
        return std::to_wstring(days) + L"d " + std::to_wstring(hours) + L"h";
    }
    if (hours > 0) {
        return std::to_wstring(hours) + L"h " + std::to_wstring(minutes) + L"m";
    }
    return std::to_wstring(minutes) + L"m";
}

std::wstring FormatUserDisplay(const UsageSnapshot& snapshot) {
    if (!snapshot.email.empty()) {
        return snapshot.email;
    }
    return L"--";
}

std::wstring BuildUsageTooltip(const UsageSnapshot& snapshot) {
    if (!snapshot.success) {
        std::wstring text;
        if (!snapshot.email.empty()) {
            text = L"\u7528\u6237: ";
            text += snapshot.email;
            text += L"\n";
        }
        if (snapshot.errorMessage.empty()) {
            text += L"Cursor Usage: no data";
        } else {
            text += L"Cursor Usage error: ";
            text += snapshot.errorMessage;
        }
        return text;
    }

    const auto formatWindow = [](const wchar_t* title, const UsageWindow& window) {
        if (window.remainingPercent < 0) {
            return std::wstring(title) + L" --";
        }
        std::wstring text = title;
        text += FormatRemainingPercent(window.remainingPercent);
        text += L" (已用 ";
        text += std::to_wstring(window.usedPercent);
        text += L"%";
        if (window.resetAfterSeconds > 0 || window.resetAtUnixSeconds > 0) {
            text += L"，";
            text += FormatResetAfter(window.resetAfterSeconds);
            text += L"\u540e\u91cd\u7f6e";
        }
        text += L")";
        return text;
    };

    std::wstring text;
    if (!snapshot.email.empty()) {
        text = L"\u7528\u6237: ";
        text += snapshot.email;
        text += L"\n";
    }
    text += formatWindow(L"Cursor Models \u5269\u4f59 ", snapshot.cursorModels);
    text += L"\n";
    text += formatWindow(L"Other Models \u5269\u4f59 ", snapshot.otherModels);
    if (!snapshot.planType.empty()) {
        text += L"\n\u5957\u9910: ";
        text += snapshot.planType;
    }
    if (snapshot.remainingCents >= 0 && snapshot.limitCents > 0) {
        text += L"\n\u5957\u9910\u989d\u5ea6: $";
        text += std::to_wstring(snapshot.remainingCents / 100);
        text += L".";
        const int cents = snapshot.remainingCents % 100;
        if (cents < 10) {
            text += L"0";
        }
        text += std::to_wstring(cents);
        text += L" / $";
        text += std::to_wstring(snapshot.limitCents / 100);
        text += L".";
        const int limitCents = snapshot.limitCents % 100;
        if (limitCents < 10) {
            text += L"0";
        }
        text += std::to_wstring(limitCents);
    } else if (!snapshot.displayMessage.empty()) {
        text += L"\n";
        text += snapshot.displayMessage;
    }
    return text;
}
