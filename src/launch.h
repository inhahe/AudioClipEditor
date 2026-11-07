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

// A file, as it is now, told apart from every other: read from the file itself.
struct FileIdentity {
    bool known = false;     // false: it couldn't be opened, or its file system gives no usable id
    ULONGLONG volume = 0;   // the volume's serial number
    FILE_ID_128 id{};       // the file's id on that volume
    ULONGLONG written = 0;  // last write time
    ULONGLONG size = 0;
};

// A file system's 64-bit file index as a 128-bit id: in the low bytes, or all ones
// if it is all ones (FILE_INVALID_FILE_ID), so usableFileId sees it either way.
inline FILE_ID_128 fileIdFromIndex(ULONGLONG index) {
    FILE_ID_128 id{};
    if (index == ~0ULL) std::memset(id.Identifier, 0xFF, sizeof(id.Identifier));
    else std::memcpy(id.Identifier, &index, sizeof(index));
    return id;
}
// Whether a file id names one file. Some redirectors (WebDAV, some cloud and FUSE
// mounts) give 0 for every file, and all ones is FILE_INVALID_FILE_ID: either would
// make every file on such a volume the same file.
inline bool usableFileId(const FILE_ID_128& id) {
    bool zero = true, ones = true;
    for (const BYTE b : id.Identifier) { zero = zero && b == 0; ones = ones && b == 0xFF; }
    return !zero && !ones;
}

// The identity of the file at `path`; not `known` if it can't be read.
inline FileIdentity fileIdentity(const std::wstring& path) {
    FileIdentity r;
    const HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    BY_HANDLE_FILE_INFORMATION fi{};
    if (GetFileInformationByHandle(h, &fi)) {
        r.written = ((ULONGLONG)fi.ftLastWriteTime.dwHighDateTime << 32) | fi.ftLastWriteTime.dwLowDateTime;
        r.size = ((ULONGLONG)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
        FILE_ID_INFO ii{};
        if (GetFileInformationByHandleEx(h, FileIdInfo, &ii, sizeof(ii))) {
            r.volume = ii.VolumeSerialNumber;
            r.id = ii.FileId;
        } else {   // file systems without 128-bit ids (FAT, some network shares)
            r.volume = fi.dwVolumeSerialNumber;
            r.id = fileIdFromIndex(((ULONGLONG)fi.nFileIndexHigh << 32) | fi.nFileIndexLow);
        }
        r.known = usableFileId(r.id);
    }
    CloseHandle(h);
    return r;
}

// Whether `a` and `b` are the same file, unchanged -- whether a project asked for is
// the one already open. `ia` and `ib` are their identities, read whenever suits the
// caller (fileIdentity). When both are known: the same volume and id, last written at
// the same time, at the same size. An 8.3 short name, another case, a mapped drive and
// its UNC path, a junction or a hard link all reach the same file by a different
// string; a shadow copy (Previous Versions) or a restored image keeps the volume serial
// and the id but not, once the file has been changed since, the time. Otherwise by the
// path, ignoring case the way Windows does.
inline bool sameFile(const std::wstring& a, const FileIdentity& ia,
                     const std::wstring& b, const FileIdentity& ib) {
    if (ia.known && ib.known)
        return ia.volume == ib.volume && std::memcmp(&ia.id, &ib.id, sizeof(ia.id)) == 0 &&
               ia.written == ib.written && ia.size == ib.size;
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}
inline bool sameFile(const std::wstring& a, const std::wstring& b) {
    return sameFile(a, fileIdentity(a), b, fileIdentity(b));
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
