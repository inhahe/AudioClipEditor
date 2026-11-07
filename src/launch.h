#pragma once
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

// What the app was asked to open when it started: projects given on the command
// line -- double-clicking a .acep file, "Open with", dropping it on the exe -- and
// audio files to add as clips. Kept out of ui.cpp so the selftest can check the
// rules.
namespace launch {

struct Request {
    // In the order given. Only one can be open; they are tried in turn until one
    // opens, so a missing first project doesn't leave the app empty.
    std::vector<std::wstring> projects;
    std::vector<std::wstring> audio;   // everything else that isn't a switch, in order
};

// Whether `path` names a project by its extension, case-insensitively.
inline bool isProjectPath(const std::wstring& path) {
    if (path.size() < 5) return false;
    std::wstring ext = path.substr(path.size() - 5);
    for (wchar_t& ch : ext) ch = (wchar_t)std::towlower(ch);
    return ext == L".acep";
}

// Whether `path` is a project: by its first four bytes ("ACEP") when it can be read
// -- so a project reached by an 8.3 short name (SHOW~1.ACE) or kept under another
// name (show.acep.bak) is still opened as one -- or else by its extension, so a
// missing or damaged project is reported as a project that won't open rather than
// as audio that won't decode.
inline bool looksLikeProject(const std::wstring& path) {
    if (isProjectPath(path)) return true;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char magic[4];
    const bool project = fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "ACEP", 4) == 0;
    fclose(f);
    return project;
}

// `args` are the command-line arguments after the program name. Switches ("--...")
// are the developer modes main.cpp handles before the window exists; they are not
// files. `isProject` decides what is a project (looksLikeProject in the app; a
// stand-in in tests).
template <class IsProject>
Request parse(const std::vector<std::wstring>& args, IsProject isProject) {
    Request r;
    for (const std::wstring& a : args) {
        if (a.empty() || a.rfind(L"--", 0) == 0) continue;
        (isProject(a) ? r.projects : r.audio).push_back(a);
    }
    return r;
}
inline Request parse(const std::vector<std::wstring>& args) { return parse(args, isProjectPath); }

} // namespace launch
