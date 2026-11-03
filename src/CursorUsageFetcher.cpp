#include "CursorUsageFetcher.h"

#include "CursorSafeStorage.h"
#include "CursorSqlite.h"
#include "JsonLite.h"

#include <Windows.h>
#include <winhttp.h>

#include <cwctype>

namespace {

bool EndsWithIgnoreCase(const std::wstring& value, const std::wstring& suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }
    for (size_t i = 0; i < suffix.size(); ++i) {
        const wchar_t a = towlower(value[value.size() - suffix.size() + i]);
        const wchar_t b = towlower(suffix[i]);
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> HttpExchange(const std::wstring& userAgent,
                                        const std::wstring& host,
                                        const std::wstring& path,
                                        const std::wstring& method,
                                        const std::vector<std::wstring>& headers,
                                        const std::string& body,
                                        const CursorUsageConfig& config,
                                        DWORD* statusCodeOut,
                                        std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (statusCodeOut != nullptr) {
        *statusCodeOut = 0;
    }

    const DWORD accessType = config.proxyMode == ProxyMode::Manual ? WINHTTP_ACCESS_TYPE_NAMED_PROXY :
        config.proxyMode == ProxyMode::Direct ? WINHTTP_ACCESS_TYPE_NO_PROXY : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    HINTERNET session = WinHttpOpen(userAgent.c_str(), accessType,
        config.proxyMode == ProxyMode::Manual ? config.proxyServer.c_str() : WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"WinHttpOpen failed";
        }
        return std::nullopt;
    }

    std::optional<std::string> responseBody;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;

    do {
        connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connect == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpConnect failed";
            }
            break;
        }

        request = WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpOpenRequest failed";
            }
            break;
        }

        for (const std::wstring& header : headers) {
            if (!WinHttpAddRequestHeaders(request, header.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpAddRequestHeaders failed";
                }
                break;
            }
        }
        if (errorMessage != nullptr && !errorMessage->empty()) {
            break;
        }

        constexpr DWORD timeout = 15000;
        WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);

        LPVOID bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : reinterpret_cast<LPVOID>(const_cast<char*>(body.data()));
        const DWORD bodySize = static_cast<DWORD>(body.size());
        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyPtr, bodySize, bodySize, 0)) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpSendRequest failed";
            }
            break;
        }

        if (!WinHttpReceiveResponse(request, nullptr)) {
            if (errorMessage != nullptr) {
                *errorMessage = L"WinHttpReceiveResponse failed";
            }
            break;
        }

        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
        if (statusCodeOut != nullptr) {
            *statusCodeOut = statusCode;
        }
        if (statusCode != 200) {
            if (errorMessage != nullptr) {
                *errorMessage = host + path + L" returned HTTP " + std::to_wstring(statusCode);
                if (statusCode == 401) {
                    *errorMessage += L"; Cursor access token may be expired";
                }
            }
            break;
        }

        std::string response;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpQueryDataAvailable failed";
                }
                break;
            }
            if (available == 0) {
                responseBody = std::move(response);
                break;
            }

            std::string chunk(static_cast<size_t>(available), '\0');
            DWORD downloaded = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &downloaded)) {
                if (errorMessage != nullptr) {
                    *errorMessage = L"WinHttpReadData failed";
                }
                break;
            }

            chunk.resize(downloaded);
            response.append(chunk);
        }
    } while (false);

    if (request != nullptr) {
        WinHttpCloseHandle(request);
    }
    if (connect != nullptr) {
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);

    return responseBody;
}

}  // namespace

UsageSnapshot CursorUsageFetcher::Fetch(const CursorUsageConfig& input) const {
    auto config = input;
    UsageSnapshot snapshot;

    std::wstring errorMessage;
    if (!NormalizeCursorConfig(&config, &errorMessage)) {
        snapshot.errorMessage = errorMessage;
        return snapshot;
    }

    std::optional<AuthTokens> tokens = ReadAuthTokens(config, &errorMessage);
    if (!tokens.has_value()) {
        snapshot.errorMessage = errorMessage;
        return snapshot;
    }

    std::optional<std::string> usageJson = HttpPostUsageJson(tokens->accessToken, config, &errorMessage);
    if (!usageJson.has_value() && errorMessage.find(L"HTTP 401") != std::wstring::npos && !tokens->refreshToken.empty()) {
        std::wstring refreshError;
        std::optional<std::string> refreshed = TryRefreshAccessToken(tokens->refreshToken, config, &refreshError);
        if (refreshed.has_value()) {
            tokens->accessToken = *refreshed;
            usageJson = HttpPostUsageJson(tokens->accessToken, config, &errorMessage);
        } else if (errorMessage.empty()) {
            errorMessage = refreshError;
        }
    }

    if (!usageJson.has_value()) {
        snapshot.email = tokens->email;
        snapshot.planType = tokens->planType;
        snapshot.errorMessage = errorMessage;
        return snapshot;
    }

    snapshot = ParseUsageJson(*usageJson, &errorMessage);
    if (snapshot.email.empty()) {
        snapshot.email = tokens->email;
    }
    if (snapshot.planType.empty()) {
        snapshot.planType = tokens->planType;
    }
    if (!snapshot.success) {
        snapshot.errorMessage = errorMessage;
    }
    return snapshot;
}

std::optional<CursorUsageFetcher::AuthTokens> CursorUsageFetcher::ReadAuthTokens(const CursorUsageConfig& config,
                                                                                   std::wstring* errorMessage) const {
    const auto candidates = ResolveCursorAuthCandidates(config);
    if (candidates.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"no Cursor auth path candidates";
        }
        return std::nullopt;
    }

    std::wstring lastError;
    std::wstring cachedEmail;
    std::wstring cachedPlan;
    bool sawEncryptedToken = false;

    for (const auto& authPath : candidates) {
        std::wstring attemptError;
        auto tokens = ReadAuthTokensFromPath(authPath, &attemptError);
        if (!tokens.has_value()) {
            if (attemptError.find(L"encrypted at rest") != std::wstring::npos ||
                attemptError.find(L"safe-storage") != std::wstring::npos ||
                attemptError.find(L"decrypt") != std::wstring::npos) {
                sawEncryptedToken = true;
                // Email/plan stay plaintext in state.vscdb even when tokens are sealed.
                if (EndsWithIgnoreCase(authPath, L".vscdb")) {
                    if (auto email = ReadCursorSqliteItem(authPath, "cursorAuth/cachedEmail", nullptr); email.has_value()) {
                        cachedEmail = Utf8ToWide(*email);
                    }
                    if (auto plan = ReadCursorSqliteItem(authPath, "cursorAuth/stripeMembershipType", nullptr);
                        plan.has_value()) {
                        cachedPlan = Utf8ToWide(*plan);
                    }
                }
            }
            lastError = attemptError;
            continue;
        }

        if (tokens->email.empty()) {
            tokens->email = cachedEmail;
        } else {
            cachedEmail = tokens->email;
        }
        if (tokens->planType.empty()) {
            tokens->planType = cachedPlan;
        } else {
            cachedPlan = tokens->planType;
        }

        if (errorMessage != nullptr) {
            errorMessage->clear();
        }
        return tokens;
    }

    if (errorMessage != nullptr) {
        if (candidates.size() == 1) {
            *errorMessage = lastError;
        } else if (sawEncryptedToken) {
            *errorMessage =
                L"Cursor desktop encrypts auth tokens (cursor-safe-storage); "
                L"decryption failed and no usable auth.json fallback was found. "
                L"last error: " + lastError;
        } else {
            *errorMessage = L"cannot find Cursor auth in default locations; last error: " + lastError;
        }
    }
    return std::nullopt;
}

std::optional<CursorUsageFetcher::AuthTokens> CursorUsageFetcher::ReadAuthTokensFromPath(
    const std::wstring& authPath, std::wstring* errorMessage) const {
    AuthTokens tokens;
    tokens.sourcePath = authPath;

    auto finalizeTokens = [&](AuthTokens candidate) -> std::optional<AuthTokens> {
        std::wstring resolveError;
        auto access = ResolveCursorAccessToken(candidate.accessToken, authPath, &resolveError);
        if (!access.has_value()) {
            if (errorMessage != nullptr) {
                if (IsCursorSafeStorageToken(candidate.accessToken)) {
                    *errorMessage = L"Cursor token is encrypted at rest (" + authPath + L"): " + resolveError;
                } else {
                    *errorMessage = resolveError.empty() ? L"unusable Cursor access token" : resolveError;
                }
            }
            return std::nullopt;
        }
        candidate.accessToken = std::move(*access);

        if (!candidate.refreshToken.empty() && IsCursorSafeStorageToken(candidate.refreshToken)) {
            std::wstring refreshError;
            auto refresh = ResolveCursorAccessToken(candidate.refreshToken, authPath, &refreshError);
            if (refresh.has_value()) {
                candidate.refreshToken = std::move(*refresh);
            } else {
                // Access token may still work; drop unusable encrypted refresh token.
                candidate.refreshToken.clear();
            }
        }
        return candidate;
    };

    if (EndsWithIgnoreCase(authPath, L".vscdb")) {
        std::optional<std::string> access = ReadCursorSqliteItem(authPath, "cursorAuth/accessToken", errorMessage);
        if (!access.has_value()) {
            return std::nullopt;
        }
        tokens.accessToken = *access;
        if (auto refresh = ReadCursorSqliteItem(authPath, "cursorAuth/refreshToken", nullptr); refresh.has_value()) {
            tokens.refreshToken = *refresh;
        }
        if (auto email = ReadCursorSqliteItem(authPath, "cursorAuth/cachedEmail", nullptr); email.has_value()) {
            tokens.email = Utf8ToWide(*email);
        }
        if (auto plan = ReadCursorSqliteItem(authPath, "cursorAuth/stripeMembershipType", nullptr); plan.has_value()) {
            tokens.planType = Utf8ToWide(*plan);
        }
        return finalizeTokens(std::move(tokens));
    }

    std::optional<std::string> jsonText = LoadFileUtf8(authPath, errorMessage);
    if (!jsonText.has_value()) {
        return std::nullopt;
    }

    jsonlite::Parser parser(*jsonText);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"auth file parse failed: " + Utf8ToWide(parser.Error());
        }
        return std::nullopt;
    }

    const jsonlite::Value* accessTokenNode = root->Find("accessToken");
    if (accessTokenNode == nullptr) {
        const jsonlite::Value* nestedTokens = root->Find("tokens");
        accessTokenNode = nestedTokens != nullptr ? nestedTokens->Find("access_token") : nullptr;
        if (accessTokenNode == nullptr && nestedTokens != nullptr) {
            accessTokenNode = nestedTokens->Find("accessToken");
        }
    }
    auto access = accessTokenNode != nullptr ? accessTokenNode->AsString() : std::nullopt;
    if (!access.has_value() || access->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"auth file missing accessToken";
        }
        return std::nullopt;
    }
    tokens.accessToken = std::string(*access);

    if (const jsonlite::Value* refreshNode = root->Find("refreshToken"); refreshNode != nullptr) {
        if (auto refresh = refreshNode->AsString(); refresh.has_value()) {
            tokens.refreshToken = std::string(*refresh);
        }
    }
    if (const jsonlite::Value* emailNode = root->Find("email"); emailNode != nullptr) {
        if (auto email = emailNode->AsString(); email.has_value()) {
            tokens.email = Utf8ToWide(std::string(*email));
        }
    }
    return finalizeTokens(std::move(tokens));
}

std::optional<std::string> CursorUsageFetcher::LoadFileUtf8(const std::wstring& path, std::wstring* errorMessage) const {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot open " + path;
        }
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot read " + path;
        }
        return std::nullopt;
    }

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = contents.empty() || ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != contents.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot read " + path;
        }
        return std::nullopt;
    }

    return contents;
}

std::optional<std::string> CursorUsageFetcher::HttpPostUsageJson(const std::string& accessToken,
                                                                  const CursorUsageConfig& config,
                                                                  std::wstring* errorMessage) const {
    DWORD statusCode = 0;
    auto body = HttpExchange(
        L"TrafficMonitorCursorUsage/0.1",
        L"api2.cursor.sh",
        L"/aiserver.v1.DashboardService/GetCurrentPeriodUsage",
        L"POST",
        {
            L"Authorization: Bearer " + Utf8ToWide(accessToken),
            L"Content-Type: application/json",
            L"Connect-Protocol-Version: 1",
        },
        "{}",
        config,
        &statusCode,
        errorMessage);
    return body;
}

std::optional<std::string> CursorUsageFetcher::TryRefreshAccessToken(const std::string& refreshToken,
                                                                      const CursorUsageConfig& config,
                                                                      std::wstring* errorMessage) const {
    const std::string body =
        std::string("{\"grant_type\":\"refresh_token\",\"client_id\":\"KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB\",\"refresh_token\":\"") +
        refreshToken + "\"}";

    DWORD statusCode = 0;
    auto response = HttpExchange(
        L"TrafficMonitorCursorUsage/0.1",
        L"api2.cursor.sh",
        L"/oauth/token",
        L"POST",
        { L"Content-Type: application/json" },
        body,
        config,
        &statusCode,
        errorMessage);
    if (!response.has_value()) {
        return std::nullopt;
    }

    jsonlite::Parser parser(*response);
    std::optional<jsonlite::Value> root = parser.Parse();
    if (!root.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"token refresh JSON parse failed";
        }
        return std::nullopt;
    }

    if (const jsonlite::Value* shouldLogout = root->Find("shouldLogout"); shouldLogout != nullptr) {
        if (auto flag = shouldLogout->AsBool(); flag.has_value() && *flag) {
            if (errorMessage != nullptr) {
                *errorMessage = L"Cursor refresh token invalid; please sign in again in Cursor";
            }
            return std::nullopt;
        }
    }

    const jsonlite::Value* access = root->Find("access_token");
    auto token = access != nullptr ? access->AsString() : std::nullopt;
    if (!token.has_value() || token->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"token refresh response missing access_token";
        }
        return std::nullopt;
    }
    return std::string(*token);
}
