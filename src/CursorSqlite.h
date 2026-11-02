#pragma once

#include <optional>
#include <string>

// Read a UTF-8 text value from VS Code / Cursor ItemTable in state.vscdb.
std::optional<std::string> ReadCursorSqliteItem(const std::wstring& dbPath, const char* key, std::wstring* errorMessage);
