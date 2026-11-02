#pragma once

#include "CursorUsageCore.h"
#include "CursorUsageConfig.h"

#include <optional>
#include <string>
#include <vector>

class CursorUsageFetcher {
public:
    UsageSnapshot Fetch(const CursorUsageConfig& config = {}) const;

private:
    struct AuthTokens {
        std::string accessToken;
        std::string refreshToken;
        std::wstring email;
        std::wstring planType;
        std::wstring sourcePath;
    };

    std::optional<AuthTokens> ReadAuthTokens(const CursorUsageConfig& config, std::wstring* errorMessage) const;
    std::optional<AuthTokens> ReadAuthTokensFromPath(const std::wstring& authPath, std::wstring* errorMessage) const;
    std::optional<std::string> LoadFileUtf8(const std::wstring& path, std::wstring* errorMessage) const;
    std::optional<std::string> HttpPostUsageJson(const std::string& accessToken, const CursorUsageConfig& config,
                                                 std::wstring* errorMessage) const;
    std::optional<std::string> TryRefreshAccessToken(const std::string& refreshToken, const CursorUsageConfig& config,
                                                     std::wstring* errorMessage) const;
};
