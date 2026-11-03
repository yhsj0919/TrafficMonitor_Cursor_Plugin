#include <winsock2.h>
#include "CursorUsageConfig.h"
#include "CursorUsageFetcher.h"
#include "CursorSafeStorage.h"
#include "CursorSqlite.h"
#include "sqlite3.h"
#include <Windows.h>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void CheckManualProxy(const std::wstring& authPath) {
    WSADATA data{};
    Check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "start local proxy test");
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Check(listener != INVALID_SOCKET, "create local proxy socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0,
          "listen on loopback proxy");
    int length = sizeof(address);
    Check(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0, "get local proxy port");
    std::string request;
    std::thread proxy([&]() {
        fd_set sockets;
        FD_ZERO(&sockets);
        FD_SET(listener, &sockets);
        timeval timeout{5, 0};
        if (select(0, &sockets, nullptr, nullptr, &timeout) > 0) {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client != INVALID_SOCKET) {
                DWORD receiveTimeout = 5000;
                setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
                char buffer[4096]{};
                const int size = recv(client, buffer, sizeof(buffer), 0);
                if (size > 0) request.assign(buffer, size);
                const std::string reply = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                send(client, reply.data(), static_cast<int>(reply.size()), 0);
                closesocket(client);
            }
        }
    });
    const std::string auth = R"({"accessToken":"synthetic-test-token","refreshToken":"synthetic-refresh"})";
    HANDLE file = CreateFileW(authPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create synthetic auth file");
    DWORD written = 0;
    Check(WriteFile(file, auth.data(), static_cast<DWORD>(auth.size()), &written, nullptr) && written == auth.size(), "write synthetic token");
    CloseHandle(file);
    CursorUsageConfig config;
    config.authPath = authPath;
    config.proxyMode = ProxyMode::Manual;
    config.proxyServer = L"127.0.0.1:" + std::to_wstring(ntohs(address.sin_port));
    const auto snapshot = CursorUsageFetcher{}.Fetch(config);
    proxy.join();
    closesocket(listener);
    WSACleanup();
    DeleteFileW(authPath.c_str());
    Check(!snapshot.success, "proxy error is surfaced");
    Check(request.find("CONNECT api2.cursor.sh:443 ") != std::string::npos, "usage fetch reaches configured proxy using HTTPS CONNECT");
    Check(request.find("synthetic-test-token") == std::string::npos, "token is not sent in plaintext to proxy");
}

void CheckSqliteAuthRead(const std::wstring& dbPath) {
    sqlite3* db = nullptr;
    Check(sqlite3_open16(dbPath.c_str(), &db) == SQLITE_OK, "create temp sqlite db");
    Check(sqlite3_exec(db,
                       "CREATE TABLE ItemTable (key TEXT PRIMARY KEY, value BLOB);"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/accessToken', 'sqlite-access-token');"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/refreshToken', 'sqlite-refresh-token');"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/cachedEmail', 'user@example.com');"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/stripeMembershipType', 'ultra');",
                       nullptr, nullptr, nullptr) == SQLITE_OK,
          "seed ItemTable");
    sqlite3_close(db);

    std::wstring error;
    auto access = ReadCursorSqliteItem(dbPath, "cursorAuth/accessToken", &error);
    Check(access.has_value() && *access == "sqlite-access-token", "read access token from sqlite");
    auto email = ReadCursorSqliteItem(dbPath, "cursorAuth/cachedEmail", &error);
    Check(email.has_value() && *email == "user@example.com", "read email from sqlite");
    DeleteFileW(dbPath.c_str());
}

void WriteUtf8File(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(file != INVALID_HANDLE_VALUE, "create utf8 auth file");
    DWORD written = 0;
    Check(WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size(),
          "write utf8 auth file");
    CloseHandle(file);
}

void CheckEncryptedTokenFallback(const std::wstring& basePath) {
    Check(IsCursorSafeStorageToken("cursor-safe-storage:v1:AAAA"), "detect sealed cursor token prefix");
    Check(!IsCursorSafeStorageToken("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.a.b"), "plaintext JWT is not sealed");

    std::wstring resolveError;
    auto plain = ResolveCursorAccessToken("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.a.b", L"", &resolveError);
    Check(plain.has_value() && *plain == "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.a.b", "plaintext token passes through");
    Check(!ResolveCursorAccessToken("cursor-safe-storage:v1:not-a-real-blob", L"", &resolveError).has_value(),
          "reject undecryptable sealed token");
    Check(!resolveError.empty(), "undecryptable sealed token sets error");

    const std::wstring dbPath = basePath + L".encrypted.vscdb";
    const std::wstring authJsonPath = basePath + L".fallback.auth.json";

    sqlite3* db = nullptr;
    Check(sqlite3_open16(dbPath.c_str(), &db) == SQLITE_OK, "create encrypted-token sqlite db");
    Check(sqlite3_exec(db,
                       "CREATE TABLE ItemTable (key TEXT PRIMARY KEY, value BLOB);"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/accessToken', 'cursor-safe-storage:v1:not-a-real-blob');"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/refreshToken', 'cursor-safe-storage:v1:not-a-real-blob');"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/cachedEmail', 'sealed@example.com');"
                       "INSERT INTO ItemTable VALUES ('cursorAuth/stripeMembershipType', 'pro');",
                       nullptr, nullptr, nullptr) == SQLITE_OK,
          "seed encrypted ItemTable");
    sqlite3_close(db);

    WriteUtf8File(authJsonPath, R"({"accessToken":"fallback-jwt-token","refreshToken":"fallback-refresh"})");

    // Custom path points only at the sealed DB: must fail instead of sending ciphertext as Bearer.
    CursorUsageConfig sealedOnly;
    sealedOnly.authPath = dbPath;
    sealedOnly.proxyMode = ProxyMode::Direct;
    auto sealedSnapshot = CursorUsageFetcher{}.Fetch(sealedOnly);
    Check(!sealedSnapshot.success, "sealed-only auth fails");
    Check(sealedSnapshot.errorMessage.find(L"encrypted at rest") != std::wstring::npos ||
              sealedSnapshot.errorMessage.find(L"safe-storage") != std::wstring::npos ||
              sealedSnapshot.errorMessage.find(L"decrypt") != std::wstring::npos,
          "sealed-only auth reports encryption");

    CursorUsageConfig jsonConfig;
    jsonConfig.authPath = authJsonPath;
    jsonConfig.proxyMode = ProxyMode::Manual;
    jsonConfig.proxyServer = L"127.0.0.1:1";
    auto jsonSnapshot = CursorUsageFetcher{}.Fetch(jsonConfig);
    Check(!jsonSnapshot.success, "synthetic fallback token cannot reach API");
    Check(jsonSnapshot.errorMessage.find(L"missing accessToken") == std::wstring::npos, "fallback auth.json is readable");
    Check(jsonSnapshot.errorMessage.find(L"encrypted at rest") == std::wstring::npos, "fallback auth.json is not sealed");

    DeleteFileW(dbPath.c_str());
    DeleteFileW(authJsonPath.c_str());
}
}

void RunCursorConfigTests() {
    wchar_t temp[MAX_PATH]{};
    wchar_t file[MAX_PATH]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0 && GetTempFileNameW(temp, L"cux", 0, file), "temporary config path");
    const std::wstring path = file;
    DeleteFileW(file);
    auto config = LoadCursorConfig(path);
    Check(config.authPath.empty() && config.proxyMode == ProxyMode::Direct, "missing config defaults to no proxy");
    Check(CursorUsageConfig{}.proxyMode == ProxyMode::Direct, "new configuration defaults to no proxy");
    Check(WritePrivateProfileStringW(L"CursorUsage", L"auth_path", L"custom-auth.json", path.c_str()), "create config without proxy mode");
    Check(LoadCursorConfig(path).proxyMode == ProxyMode::Direct, "missing proxy mode defaults to no proxy");
    Check(WritePrivateProfileStringW(L"CursorUsage", L"proxy_mode", L"unknown", path.c_str()), "write unknown proxy mode");
    Check(LoadCursorConfig(path).proxyMode == ProxyMode::Direct, "unknown proxy mode defaults to no proxy");
    DeleteFileW(file);
    std::wstring error;
    config.authPath = L" C:\\用户目录\\自定义 state.vscdb ";
    config.proxyMode = ProxyMode::Manual;
    config.proxyServer = L" http://127.0.0.1:7890/ ";
    Check(SaveCursorConfig(config, &error, path), "save Unicode configuration");
    config = LoadCursorConfig(path);
    Check(config.authPath == L"C:\\用户目录\\自定义 state.vscdb", "Unicode path roundtrip");
    Check(config.proxyMode == ProxyMode::Manual && config.proxyServer == L"127.0.0.1:7890", "manual proxy roundtrip");
    Check(ResolveCursorAuthPath(config) == config.authPath, "custom auth path overrides defaults");
    for (const auto value : {L"", L"socks5://127.0.0.1:1080", L"https://localhost:443", L"http://user:pass@host:80",
                             L"localhost:0", L"localhost:99999", L"http://host:80/path", L"http://host:80?query", L"a;b:80"}) {
        auto invalid = config;
        invalid.proxyServer = value;
        Check(!SaveCursorConfig(invalid, &error, path) && !error.empty(), "reject invalid manual proxy");
        Check(LoadCursorConfig(path).proxyServer == config.proxyServer, "invalid configuration preserves saved values");
    }
    config.proxyServer = L"[::1]:7890";
    Check(NormalizeCursorConfig(&config, &error) && config.proxyServer == L"[::1]:7890", "IPv6 proxy");
    for (auto mode : {ProxyMode::System, ProxyMode::Direct}) {
        config.proxyMode = mode;
        config.proxyServer.clear();
        Check(SaveCursorConfig(config, &error, path) && LoadCursorConfig(path).proxyMode == mode, "system/direct proxy roundtrip");
    }
    config.authPath = L"bad\r\nproxy_mode=manual";
    Check(!SaveCursorConfig(config, &error, path), "reject INI line injection");
    config.authPath.clear();
    Check(!SaveCursorConfig(config, &error, path + L"\\missing.ini"), "save failure is reported");

    config.authPath.clear();
    const auto defaultPath = ResolveCursorAuthPath(config);
    Check(defaultPath.find(L"Cursor\\User\\globalStorage\\state.vscdb") != std::wstring::npos,
          "blank path prefers Cursor state.vscdb");
    const auto candidates = ResolveCursorAuthCandidates(config);
    Check(candidates.size() >= 2, "blank path probes multiple default auth locations");
    bool hasAuthJson = false;
    for (const auto& candidate : candidates) {
        if (candidate.find(L"Cursor\\auth.json") != std::wstring::npos ||
            candidate.find(L".cursor\\auth.json") != std::wstring::npos) {
            hasAuthJson = true;
            break;
        }
    }
    Check(hasAuthJson, "blank path includes user auth.json candidates");
    Check(ResolveCursorAuthCandidates(CursorUsageConfig{path + L".only.json", ProxyMode::Direct, {}}).size() == 1,
          "custom auth path disables fallback candidates");

    config.authPath = path + L".missing-auth.json";
    auto snapshot = CursorUsageFetcher{}.Fetch(config);
    Check(!snapshot.success && snapshot.errorMessage.find(config.authPath) != std::wstring::npos,
          "fetcher reads custom auth path and reports missing file");
    CheckManualProxy(path + L".auth.json");
    CheckSqliteAuthRead(path + L".state.vscdb");
    CheckEncryptedTokenFallback(path);
    DeleteFileW(file);
}
