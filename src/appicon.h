#pragma once
#include <windows.h>
#include <commctrl.h>
#include "resource.h"

// Give a window the app's title-bar and taskbar icons, at the sizes the DPI of the
// monitor it is on calls for -- set again when it moves to one with another
// (WM_DPICHANGED), since a class icon is one size for the whole session. `release`
// only takes them back (the window is going) and destroys them: WM_SETICON doesn't
// copy an icon, so a window's icons must outlive their use and are the caller's to
// free.
inline void setWindowIcons(HWND hwnd, bool release = false) {
    HICON smallIcon = nullptr, bigIcon = nullptr;
    if (!release) {
        const UINT dpi = GetDpiForWindow(hwnd);
        const HINSTANCE inst = GetModuleHandleW(nullptr);
        LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                              GetSystemMetricsForDpi(SM_CYSMICON, dpi), &smallIcon);
        LoadIconWithScaleDown(inst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetricsForDpi(SM_CXICON, dpi),
                              GetSystemMetricsForDpi(SM_CYICON, dpi), &bigIcon);
    }
    const HICON oldSmall = (HICON)SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    const HICON oldBig = (HICON)SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)bigIcon);
    if (oldSmall) DestroyIcon(oldSmall);
    if (oldBig) DestroyIcon(oldBig);
}
