#pragma once

#include <optional>
#include <string>
#include <string_view>

// Cursor 3.27+ may encrypt cursorAuth tokens in state.vscdb as:
//   cursor-safe-storage:v1:<base64(Electron safeStorage blob)>
// Sending the ciphertext as a Bearer token yields HTTP 401.

bool IsCursorSafeStorageToken(std::string_view token);

// Returns a usable JWT/access token. Plaintext tokens pass through.
// Encrypted tokens are decrypted on Windows via Local State os_crypt + DPAPI/AES-GCM.
// On failure, returns nullopt and sets a diagnostic errorMessage when provided.
std::optional<std::string> ResolveCursorAccessToken(const std::string& token,
                                                    const std::wstring& authPathHint,
                                                    std::wstring* errorMessage);
