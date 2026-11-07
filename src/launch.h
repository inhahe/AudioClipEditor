#pragma once
#include <windows.h>
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

// Whether `a` and `b` name the same file -- whether a project asked for is the one
// already open. By the file itself, its volume and its id there, when both can be
// opened: an 8.3 short name, another case, a mapped drive and its UNC path, a junction
// or a hard link all reach the same file by a different string. Otherwise (one is
// missing, say) by the path, ignoring case the way Windows does.
inline bool sameFile(const std::wstring& a, const std::wstring& b) {
    auto open = [](const std::wstring& p) {
        return CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    };
    const HANDLE ha = open(a), hb = open(b);
    int same = -1;   // not known from the files
    if (ha != INVALID_HANDLE_VALUE && hb != INVALID_HANDLE_VALUE) {
        FILE_ID_INFO ia{}, ib{};
        BY_HANDLE_FILE_INFORMATION fa{}, fb{};
        if (GetFileInformationByHandleEx(ha, FileIdInfo, &ia, sizeof(ia)) &&
            GetFileInformationByHandleEx(hb, FileIdInfo, &ib, sizeof(ib)))
            same = ia.VolumeSerialNumber == ib.VolumeSerialNumber &&
                   std::memcmp(&ia.FileId, &ib.FileId, sizeof(ia.FileId)) == 0;
        else if (GetFileInformationByHandle(ha, &fa) && GetFileInformationByHandle(hb, &fb))
            // File systems without 128-bit ids (FAT, some network shares).
            same = fa.dwVolumeSerialNumber == fb.dwVolumeSerialNumber &&
                   fa.nFileIndexHigh == fb.nFileIndexHigh && fa.nFileIndexLow == fb.nFileIndexLow;
    }
    if (ha != INVALID_HANDLE_VALUE) CloseHandle(ha);
    if (hb != INVALID_HANDLE_VALUE) CloseHandle(hb);
    if (same >= 0) return same == 1;
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

// Whether a command-line argument names a file. Switches ("--...") are the developer
// modes main.cpp handles before the window exists.
inline bool isFileArg(const std::wstring& arg) { return !arg.empty() && arg.rfind(L"--", 0) != 0; }

// `args` are the command-line arguments after the program name; only the files among
// them count (isFileArg). `isProject` decides what is a project (looksLikeProject in
// the app; a stand-in in tests).
template <class IsProject>
Request parse(const std::vector<std::wstring>& args, IsProject isProject) {
    Request r;
    for (const std::wstring& a : args)
        if (isFileArg(a)) (isProject(a) ? r.projects : r.audio).push_back(a);
    return r;
}
inline Request parse(const std::vector<std::wstring>& args) { return parse(args, isProjectPath); }

// `args` written as the arguments part of a command line, which CommandLineToArgvW
// splits back into exactly `args`: each one quoted, a quote inside escaped, and the
// backslashes before a quote (or before the closing quote, as in "C:\") doubled.
inline std::wstring commandLine(const std::vector<std::wstring>& args) {
    std::wstring out;
    for (const std::wstring& a : args) {
        if (!out.empty()) out += L' ';
        out += L'"';
        size_t slashes = 0;
        for (const wchar_t ch : a) {
            if (ch == L'\\') { ++slashes; continue; }
            if (ch == L'"') slashes = slashes * 2 + 1;
            out.append(slashes, L'\\');
            slashes = 0;
            out += ch;
        }
        out.append(slashes * 2, L'\\');
        out += L'"';
    }
    return out;
}

} // namespace launch
