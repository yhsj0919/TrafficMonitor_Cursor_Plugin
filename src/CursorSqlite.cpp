#include "CursorSqlite.h"

#include "CursorUsageCore.h"

#include "sqlite3.h"

#include <Windows.h>

#include <cstdio>
#include <vector>

namespace {

std::wstring ShortenSqliteError(const char* message) {
    if (message == nullptr || message[0] == '\0') {
        return L"sqlite error";
    }
    return Utf8ToWide(message);
}

}  // namespace

std::optional<std::string> ReadCursorSqliteItem(const std::wstring& dbPath, const char* key, std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (dbPath.empty() || key == nullptr || key[0] == '\0') {
        if (errorMessage != nullptr) {
            *errorMessage = L"invalid sqlite path or key";
        }
        return std::nullopt;
    }

    // Prefer URI immutable/ro so Cursor can keep the DB open while we read.
    const std::string utf8Path = WideToUtf8(dbPath);
    if (utf8Path.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot convert sqlite path";
        }
        return std::nullopt;
    }

    std::string uri = "file:";
    for (unsigned char ch : utf8Path) {
        if (ch == ' ' || ch == '#' || ch == '?' || ch == '%' || ch < 0x20 || ch >= 0x7f) {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", ch);
            uri += hex;
        } else if (ch == '\\') {
            uri += '/';
        } else {
            uri.push_back(static_cast<char>(ch));
        }
    }
    uri += "?mode=ro&immutable=1";

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    if (rc != SQLITE_OK) {
        if (db != nullptr) {
            sqlite3_close(db);
            db = nullptr;
        }
        // Fallback without URI for older path edge cases.
        const std::wstring native = dbPath;
        std::vector<char> narrow(WideCharToMultiByte(CP_ACP, 0, native.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
        if (!narrow.empty()) {
            WideCharToMultiByte(CP_ACP, 0, native.c_str(), -1, narrow.data(), static_cast<int>(narrow.size()), nullptr, nullptr);
            rc = sqlite3_open_v2(narrow.data(), &db, SQLITE_OPEN_READONLY, nullptr);
        }
    }

    if (rc != SQLITE_OK || db == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot open " + dbPath + L": " + ShortenSqliteError(db != nullptr ? sqlite3_errmsg(db) : nullptr);
        }
        if (db != nullptr) {
            sqlite3_close(db);
        }
        return std::nullopt;
    }

    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, "SELECT value FROM ItemTable WHERE key = ?1 LIMIT 1;", -1, &stmt, nullptr);
    if (rc != SQLITE_OK || stmt == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"sqlite prepare failed: " + ShortenSqliteError(sqlite3_errmsg(db));
        }
        sqlite3_close(db);
        return std::nullopt;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    std::optional<std::string> value;
    if (rc == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        const int bytes = sqlite3_column_bytes(stmt, 0);
        if (text != nullptr && bytes > 0) {
            value = std::string(reinterpret_cast<const char*>(text), static_cast<size_t>(bytes));
        } else if (text != nullptr) {
            value = std::string();
        }
    } else if (rc != SQLITE_DONE) {
        if (errorMessage != nullptr) {
            *errorMessage = L"sqlite query failed: " + ShortenSqliteError(sqlite3_errmsg(db));
        }
    } else if (errorMessage != nullptr) {
        *errorMessage = L"state.vscdb missing key " + Utf8ToWide(key);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (!value.has_value() || value->empty()) {
        if (errorMessage != nullptr && errorMessage->empty()) {
            *errorMessage = L"state.vscdb missing key " + Utf8ToWide(key);
        }
        return std::nullopt;
    }
    return value;
}
