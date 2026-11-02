#pragma once

#include <string>
#include <vector>

enum class ProxyMode { System, Direct, Manual };

struct CursorUsageConfig {
    std::wstring authPath;
    ProxyMode proxyMode = ProxyMode::Direct;
    std::wstring proxyServer;
};

std::wstring ResolveCursorConfigPath();
CursorUsageConfig LoadCursorConfig(const std::wstring& path = ResolveCursorConfigPath());
bool SaveCursorConfig(const CursorUsageConfig& config, std::wstring* error,
                      const std::wstring& path = ResolveCursorConfigPath());
bool NormalizeCursorConfig(CursorUsageConfig* config, std::wstring* error);
// Preferred / first candidate. Custom authPath disables fallback.
std::wstring ResolveCursorAuthPath(const CursorUsageConfig& config);
// When authPath is empty: state.vscdb, then %APPDATA%\Cursor\auth.json, then %USERPROFILE%\.cursor\auth.json.
std::vector<std::wstring> ResolveCursorAuthCandidates(const CursorUsageConfig& config);
