#include "assoc.h"
#include <windows.h>

namespace assoc {

namespace {

std::wstring progKey(const std::wstring& root) { return root + L"\\" + kProgId; }
std::wstring extKey(const std::wstring& root) { return root + L"\\.acep"; }

bool setString(const std::wstring& sub, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LSTATUS s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)value.c_str(),
                                     (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return s == ERROR_SUCCESS;
}

// An empty REG_NONE value: how a ProgID is listed under a type's OpenWithProgids.
bool setNone(const std::wstring& sub, const wchar_t* name) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    const LSTATUS s = RegSetValueExW(k, name, 0, REG_NONE, nullptr, 0);
    RegCloseKey(k);
    return s == ERROR_SUCCESS;
}

// The string value, or empty if there is none (or it isn't a string).
std::wstring getString(const std::wstring& sub, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr,
                     &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return std::wstring();
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, &s[0],
                     &size) != ERROR_SUCCESS) return std::wstring();
    s.resize(wcslen(s.c_str()));
    return s;
}

// Deleting something already gone counts as done.
bool gone(LSTATUS s) { return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND; }

// Remove `sub` if it has no values and no subkeys left.
bool removeIfEmpty(const std::wstring& sub) {
    HKEY k = nullptr;
    const LSTATUS o = RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_QUERY_VALUE, &k);
    if (o == ERROR_FILE_NOT_FOUND) return true;
    if (o != ERROR_SUCCESS) return false;
    DWORD subkeys = 0, values = 0;
    const LSTATUS q = RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                                       &values, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(k);
    if (q != ERROR_SUCCESS) return false;
    if (subkeys || values) return true;   // something else lives here: leave it
    return gone(RegDeleteKeyW(HKEY_CURRENT_USER, sub.c_str()));
}

} // namespace

std::wstring openCommand(const std::wstring& exe) {
    return L"\"" + exe + L"\" \"%1\"";
}

bool projectsOpenWith(const std::wstring& root, const std::wstring& exe) {
    return getString(extKey(root), nullptr) == kProgId &&
           _wcsicmp(getString(progKey(root) + L"\\shell\\open\\command", nullptr).c_str(),
                    openCommand(exe).c_str()) == 0;
}

bool registerProjects(const std::wstring& root, const std::wstring& exe) {
    bool ok = setString(progKey(root), nullptr, L"Audio Clip Editor project");
    ok = setString(progKey(root) + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0") && ok;
    ok = setString(progKey(root) + L"\\shell\\open\\command", nullptr, openCommand(exe)) && ok;
    ok = setString(extKey(root), nullptr, kProgId) && ok;
    ok = setNone(extKey(root) + L"\\OpenWithProgids", kProgId) && ok;
    return ok;
}

bool unregisterProjects(const std::wstring& root) {
    bool ok = gone(RegDeleteTreeW(HKEY_CURRENT_USER, progKey(root).c_str()));
    if (getString(extKey(root), nullptr) == kProgId)
        ok = gone(RegDeleteKeyValueW(HKEY_CURRENT_USER, extKey(root).c_str(), nullptr)) && ok;
    ok = gone(RegDeleteKeyValueW(HKEY_CURRENT_USER, (extKey(root) + L"\\OpenWithProgids").c_str(),
                                 kProgId)) && ok;
    ok = removeIfEmpty(extKey(root) + L"\\OpenWithProgids") && ok;
    ok = removeIfEmpty(extKey(root)) && ok;
    return ok;
}

} // namespace assoc
