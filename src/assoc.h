#pragma once
#include <string>

// Opening .acep projects by double-clicking them in Explorer: the per-user file
// association, under HKEY_CURRENT_USER so it needs no administrator rights and
// touches no other user. Only ever set or removed on request (File menu), since it
// changes how Windows behaves outside the app.
namespace assoc {

// Where associations live, below HKEY_CURRENT_USER. The selftest passes a scratch
// root instead, so testing never touches the user's real associations.
constexpr const wchar_t* kClassesRoot = L"Software\\Classes";
constexpr const wchar_t* kProgId = L"AudioClipEditor.Project";
// Under the ProgID: what .acep opened with before we registered, to hand back.
constexpr const wchar_t* kPreviousValue = L"PreviousDefault";

// The command Explorer runs to open a project with `exe`: `"exe" "%1"`.
std::wstring openCommand(const std::wstring& exe);

// Where Explorer finds the icon project files show: the project icon in `exe`, by its
// resource id (`"exe",-2`, IDI_PROJECT), so the icons' order in the exe doesn't matter.
std::wstring projectIcon(const std::wstring& exe);

// Whether the registration under `root` points .acep at `exe` (this copy of the app).
// Not the same as what a double-click does: see projectsOpenHere.
bool projectsOpenWith(const std::wstring& root, const std::wstring& exe);

// What double-clicking a .acep file actually runs now -- Explorer's own answer, which
// puts a choice made with "Always use this app" (the hash-protected UserChoice, which
// no program can legitimately set) ahead of any registration. Empty if nothing is.
std::wstring effectiveOpenCommand();
// Whether double-clicking a .acep file opens it with `exe`. What the menu tick shows.
bool projectsOpenHere(const std::wstring& exe);

// Make .acep files open with `exe` for this user, remembering what they opened with
// before. All or nothing: if any key can't be written, what was written is taken out
// again and false is returned.
bool registerProjects(const std::wstring& root, const std::wstring& exe);

// Undo registerProjects. Removes this app's ProgID and, if .acep still names it, hands
// .acep back to what it opened with before (or clears it, if nothing did). If another
// app has claimed .acep since, its claim is left alone. Keys left empty are removed.
// Returns false if a key that exists couldn't be changed.
bool unregisterProjects(const std::wstring& root);

} // namespace assoc
