#include "CursorSafeStorage.h"

#include "CursorUsageCore.h"

#include <Windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace {

constexpr std::string_view kSafeStoragePrefix = "cursor-safe-storage:v1:";
constexpr unsigned char kDpapiKeyPrefix[] = {'D', 'P', 'A', 'P', 'I'};

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::optional<std::vector<unsigned char>> Base64Decode(std::string_view text) {
    if (text.empty()) {
        return std::vector<unsigned char>{};
    }

    DWORD size = 0;
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &size,
                              nullptr, nullptr)) {
        return std::nullopt;
    }

    std::vector<unsigned char> output(size);
    if (!CryptStringToBinaryA(text.data(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, output.data(), &size,
                              nullptr, nullptr)) {
        return std::nullopt;
    }
    output.resize(size);
    return output;
}

std::optional<std::vector<unsigned char>> DpapiUnprotect(const unsigned char* data, size_t size) {
    if (data == nullptr || size == 0 || size > static_cast<size_t>(MAXDWORD)) {
        return std::nullopt;
    }

    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(size);
    input.pbData = const_cast<BYTE*>(data);
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return std::nullopt;
    }

    std::vector<unsigned char> plain(output.pbData, output.pbData + output.cbData);
    if (output.pbData != nullptr) {
        LocalFree(output.pbData);
    }
    return plain;
}

std::optional<std::wstring> FindCursorLocalStatePath(const std::wstring& authPathHint) {
    auto tryPath = [](std::wstring path) -> std::optional<std::wstring> {
        if (path.empty()) {
            return std::nullopt;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return path;
        }
        return std::nullopt;
    };

    if (!authPathHint.empty()) {
        // ...\Cursor\User\globalStorage\state.vscdb -> ...\Cursor\Local State
        std::wstring cursorRoot = authPathHint;
        for (int i = 0; i < 3; ++i) {
            const size_t slash = cursorRoot.find_last_of(L"\\/");
            if (slash == std::wstring::npos) {
                cursorRoot.clear();
                break;
            }
            cursorRoot.resize(slash);
        }
        if (!cursorRoot.empty()) {
            if (auto path = tryPath(cursorRoot + L"\\Local State"); path.has_value()) {
                return path;
            }
        }
    }

    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return tryPath(std::wstring(buffer.data(), length) + L"\\Cursor\\Local State");
    }
    return std::nullopt;
}

std::optional<std::string> LoadFileUtf8(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return std::nullopt;
    }

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = contents.empty() || ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != contents.size()) {
        return std::nullopt;
    }
    return contents;
}

std::optional<std::string> ExtractJsonStringField(const std::string& json, const char* objectKey, const char* fieldKey) {
    // Minimal extraction for Local State: {"os_crypt":{...,"encrypted_key":"..."}}
    const std::string objectNeedle = std::string("\"") + objectKey + "\"";
    const size_t objectPos = json.find(objectNeedle);
    if (objectPos == std::string::npos) {
        return std::nullopt;
    }

    const std::string fieldNeedle = std::string("\"") + fieldKey + "\"";
    const size_t fieldPos = json.find(fieldNeedle, objectPos);
    if (fieldPos == std::string::npos) {
        return std::nullopt;
    }

    const size_t colon = json.find(':', fieldPos + fieldNeedle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    const size_t quote1 = json.find('"', colon + 1);
    if (quote1 == std::string::npos) {
        return std::nullopt;
    }
    const size_t quote2 = json.find('"', quote1 + 1);
    if (quote2 == std::string::npos) {
        return std::nullopt;
    }
    return json.substr(quote1 + 1, quote2 - quote1 - 1);
}

std::optional<std::vector<unsigned char>> LoadOsCryptAesKey(const std::wstring& authPathHint, std::wstring* errorMessage) {
    const auto localStatePath = FindCursorLocalStatePath(authPathHint);
    if (!localStatePath.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Cursor Local State not found for token decryption";
        }
        return std::nullopt;
    }

    const auto json = LoadFileUtf8(*localStatePath);
    if (!json.has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"cannot read Cursor Local State";
        }
        return std::nullopt;
    }

    const auto encryptedKeyB64 = ExtractJsonStringField(*json, "os_crypt", "encrypted_key");
    if (!encryptedKeyB64.has_value() || encryptedKeyB64->empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Cursor Local State missing os_crypt.encrypted_key";
        }
        return std::nullopt;
    }

    auto encryptedKey = Base64Decode(*encryptedKeyB64);
    if (!encryptedKey.has_value() || encryptedKey->size() <= sizeof(kDpapiKeyPrefix) ||
        !std::equal(std::begin(kDpapiKeyPrefix), std::end(kDpapiKeyPrefix), encryptedKey->begin())) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Cursor os_crypt.encrypted_key is invalid";
        }
        return std::nullopt;
    }

    auto key = DpapiUnprotect(encryptedKey->data() + sizeof(kDpapiKeyPrefix),
                              encryptedKey->size() - sizeof(kDpapiKeyPrefix));
    if (!key.has_value() || key->size() != 32) {
        if (errorMessage != nullptr) {
            *errorMessage = L"DPAPI unwrap of Cursor os_crypt key failed";
        }
        return std::nullopt;
    }
    return key;
}

std::optional<std::string> AesGcmDecrypt(const std::vector<unsigned char>& key,
                                         const unsigned char* nonce, ULONG nonceLen,
                                         const unsigned char* cipher, ULONG cipherLen,
                                         const unsigned char* tag, ULONG tagLen) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    std::optional<std::string> plain;

    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
            break;
        }
        if (BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != 0) {
            break;
        }
        if (BCryptGenerateSymmetricKey(algorithm, &keyHandle, nullptr, 0,
                                       const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) != 0) {
            break;
        }

        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<PUCHAR>(nonce);
        info.cbNonce = nonceLen;
        info.pbTag = const_cast<PUCHAR>(tag);
        info.cbTag = tagLen;

        std::string output(cipherLen, '\0');
        ULONG result = 0;
        const NTSTATUS status = BCryptDecrypt(keyHandle, const_cast<PUCHAR>(cipher), cipherLen, &info,
                                              nullptr, 0, reinterpret_cast<PUCHAR>(output.data()),
                                              static_cast<ULONG>(output.size()), &result, 0);
        if (status != 0) {
            break;
        }
        output.resize(result);
        plain = std::move(output);
    } while (false);

    if (keyHandle != nullptr) {
        BCryptDestroyKey(keyHandle);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    return plain;
}

std::optional<std::string> DecryptSafeStorageBlob(const std::string& token, const std::wstring& authPathHint,
                                                  std::wstring* errorMessage) {
    if (!StartsWith(token, kSafeStoragePrefix)) {
        return token;
    }

    auto blob = Base64Decode(std::string_view(token).substr(kSafeStoragePrefix.size()));
    if (!blob.has_value() || blob->size() < 3 + 12 + 16) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Cursor encrypted token blob is truncated";
        }
        return std::nullopt;
    }

    // Electron/Chromium OSCrypt Windows: "v10"/"v11" + 12-byte nonce + ciphertext + 16-byte tag.
    if (!(((*blob)[0] == 'v' && (*blob)[1] == '1' && ((*blob)[2] == '0' || (*blob)[2] == '1')))) {
        // Legacy path: version prefix + raw DPAPI payload.
        if (auto legacy = DpapiUnprotect(blob->data() + 3, blob->size() - 3); legacy.has_value() && !legacy->empty()) {
            return std::string(legacy->begin(), legacy->end());
        }
        if (errorMessage != nullptr) {
            *errorMessage = L"unsupported Cursor safe-storage envelope";
        }
        return std::nullopt;
    }

    // Prefer AES-GCM with Local State key (current Chromium/Electron Windows format).
    std::wstring aesError;
    if (auto key = LoadOsCryptAesKey(authPathHint, &aesError); key.has_value()) {
        const unsigned char* nonce = blob->data() + 3;
        const unsigned char* tag = blob->data() + blob->size() - 16;
        const unsigned char* cipher = blob->data() + 15;
        const ULONG cipherLen = static_cast<ULONG>(blob->size() - 3 - 12 - 16);
        if (auto plain = AesGcmDecrypt(*key, nonce, 12, cipher, cipherLen, tag, 16); plain.has_value() && !plain->empty()) {
            return plain;
        }
        aesError = L"AES-GCM decrypt of Cursor token failed";
    }

    // Fallback: some Electron builds DPAPI-protect the payload after v10.
    if (auto legacy = DpapiUnprotect(blob->data() + 3, blob->size() - 3); legacy.has_value() && !legacy->empty()) {
        return std::string(legacy->begin(), legacy->end());
    }

    if (errorMessage != nullptr) {
        *errorMessage = aesError.empty()
            ? L"cannot decrypt Cursor safe-storage token"
            : aesError;
    }
    return std::nullopt;
}

}  // namespace

bool IsCursorSafeStorageToken(std::string_view token) {
    return StartsWith(token, kSafeStoragePrefix);
}

std::optional<std::string> ResolveCursorAccessToken(const std::string& token,
                                                    const std::wstring& authPathHint,
                                                    std::wstring* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (token.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"empty Cursor access token";
        }
        return std::nullopt;
    }
    if (!IsCursorSafeStorageToken(token)) {
        return token;
    }
    return DecryptSafeStorageBlob(token, authPathHint, errorMessage);
}
