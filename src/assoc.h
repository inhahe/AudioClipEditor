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

// The command Explorer runs to open a project with `exe`: `"exe" "%1"`.
std::wstring openCommand(const std::wstring& exe);

// Whether .acep files open with `exe` -- this copy of the app -- for this user.
bool projectsOpenWith(const std::wstring& root, const std::wstring& exe);

// Make .acep files open with `exe` for this user. Returns false if a key couldn't be
// written.
bool registerProjects(const std::wstring& root, const std::wstring& exe);

// Undo registerProjects. Removes this app's ProgID, and .acep's default only if it
// still names that ProgID -- if another app has since claimed .acep, its claim is
// left alone. Keys left empty by that are removed. Returns false if a key that
// exists couldn't be removed.
bool unregisterProjects(const std::wstring& root);

} // namespace assoc
