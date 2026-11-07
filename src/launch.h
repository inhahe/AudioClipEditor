#pragma once
#include <string>
#include <vector>
#include <cwctype>

// What the app was asked to open when it started: a project given on the command
// line -- double-clicking a .acep file, "Open with", dropping it on the exe -- and
// audio files to add as clips. Kept out of ui.cpp so the selftest can check the
// rules.
namespace launch {

struct Request {
    std::wstring project;              // the first .acep given; empty = none
    std::vector<std::wstring> audio;   // everything else that isn't a switch, in order
    int extraProjects = 0;             // further .acep files: only one can be open
};

// Whether `path` names a project, by its extension, case-insensitively.
inline bool isProjectPath(const std::wstring& path) {
    if (path.size() < 5) return false;
    std::wstring ext = path.substr(path.size() - 5);
    for (wchar_t& ch : ext) ch = (wchar_t)std::towlower(ch);
    return ext == L".acep";
}

// `args` are the command-line arguments after the program name. Switches ("--...")
// are the developer modes main.cpp handles before the window exists; they are not
// files.
inline Request parse(const std::vector<std::wstring>& args) {
    Request r;
    for (const std::wstring& a : args) {
        if (a.empty() || a.rfind(L"--", 0) == 0) continue;
        if (!isProjectPath(a)) r.audio.push_back(a);
        else if (r.project.empty()) r.project = a;
        else ++r.extraProjects;
    }
    return r;
}

} // namespace launch
