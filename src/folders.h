#pragma once
// The folders the project dialogs start in, and the one remembered between sessions.
#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace folders {

// The folder `path` is in, without a trailing separator -- except a drive's root,
// "D:\", whose separator is the folder ("D:" alone means the drive's current folder,
// which is something else). Empty for a bare file name, and for a network share's
// root, "\\server\share": nothing above that is a folder ("\\server" isn't, and
// "\" would be the current drive's root).
inline std::wstring parentOf(const std::wstring& path) {
    const size_t s = path.find_last_of(L"\\/");
    if (s == std::wstring::npos) return std::wstring();
    auto sep = [](wchar_t c) { return c == L'\\' || c == L'/'; };
    if (path.size() > 2 && sep(path[0]) && sep(path[1])) {
        const size_t server = path.find_first_of(L"\\/", 2);   // the separator after "\\server"
        if (server == std::wstring::npos || s <= server) return std::wstring();
    }
    if (s > 0 && path[s - 1] == L':') return path.substr(0, s + 1);
    return path.substr(0, s);
}

// `dir` if it is a folder that exists, else the nearest folder above it that does --
// a project folder since renamed or deleted still starts a dialog as near to it as
// there is -- or empty when none does (a drive or share that isn't there any more).
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

// nearestExisting, given at most `timeoutMs` to answer; empty if it takes longer. A
// folder on a network share whose server has gone away answers each probe only when
// the network gives up on it, seconds later, and this is asked on the way to opening
// a dialog: the window would hang meanwhile. The probe goes on in the background and
// its answer is dropped.
inline std::wstring nearestExistingWithin(const std::wstring& dir, unsigned timeoutMs) {
    if (dir.empty()) return std::wstring();
    struct Probe {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::wstring result;
    };
    auto probe = std::make_shared<Probe>();   // outlives this call if the probe does
    std::thread([probe, dir] {
        std::wstring r = nearestExisting(dir);
        std::lock_guard<std::mutex> lk(probe->m);
        probe->result = std::move(r);
        probe->done = true;
        probe->cv.notify_one();
    }).detach();
    std::unique_lock<std::mutex> lk(probe->m);
    if (!probe->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return probe->done; }))
        return std::wstring();
    return probe->result;
}

// The folder of the last project opened or saved, kept as `value` under
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
