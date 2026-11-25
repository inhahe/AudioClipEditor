#pragma once
// The folders the project dialogs start in, and the one remembered between sessions.
#include <windows.h>
#include <string>

namespace folders {

// The folder `path` is in, without a trailing separator -- except a drive's root,
// "D:\", whose separator is the folder ("D:" alone means the drive's current folder,
// which is something else). Empty for a bare file name.
inline std::wstring parentOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    if (s == std::wstring::npos) return std::wstring();
    if (s > 0 && path[s - 1] == L':') return path.substr(0, s + 1);
    return path.substr(0, s);
}

// `dir` if it is a folder that exists, else the nearest folder above it that does --
// a project folder since renamed or deleted still starts a dialog as near to it as
// there is -- or empty when none does (a drive that isn't there any more).
inline std::wstring nearestExisting(std::wstring dir) {
    while (!dir.empty()) {
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return dir;
        const std::wstring up = parentOf(dir);
        if (up == dir) break;   // a root that isn't there
        dir = up;
    }
    return std::wstring();
}

// The folder the last project was loaded from, kept as `value` under
// HKEY_CURRENT_USER\<key> so the next session starts there too. Empty if none was
// ever stored or it can't be read.
inline std::wstring remembered(const std::wstring& key, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes)
            != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return std::wstring();
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, &s[0], &bytes)
            != ERROR_SUCCESS)
        return std::wstring();
    s.resize(wcsnlen(s.c_str(), s.size()));   // the stored terminator, and any slack
    return s;
}
inline void remember(const std::wstring& key, const wchar_t* value, const std::wstring& dir) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)
            != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, value, 0, REG_SZ, (const BYTE*)dir.c_str(), (DWORD)((dir.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

}  // namespace folders
