#pragma once
#include <windows.h>
#include <string>

// Where this copy of the program is -- for the file association, which must name
// it, and for the selftest's log beside it. Long paths included: GetModuleFileName
// truncates to the buffer it is given, and an install deeper than MAX_PATH is legal.
inline std::wstring exePath() {
    std::wstring p(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &p[0], (DWORD)p.size());
    p.resize(n < p.size() ? n : 0);
    return p;
}
// Its folder, without a trailing separator.
inline std::wstring exeDir() {
    const std::wstring p = exePath();
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(0, s);
}
