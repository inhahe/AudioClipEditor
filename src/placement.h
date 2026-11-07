#pragma once
#include <windows.h>

// How the main window first appears: the size, place and show state it had when it
// last closed, shown the way the launcher asked (nCmdShow) when that asks for
// something in particular. Kept out of ui.cpp so the selftest can check the rules.
//
// `saved` is the placement stored at the last close (GetWindowPlacement); `nCmdShow`
// is what wWinMain was given. Returns the placement to pass to SetWindowPlacement.
inline WINDOWPLACEMENT startupPlacement(WINDOWPLACEMENT saved, int nCmdShow) {
    // Closed while minimized: open the way it would have been restored -- maximized
    // if it was maximized before it was minimized.
    if (saved.showCmd == SW_SHOWMINIMIZED || saved.showCmd == SW_MINIMIZE ||
        saved.showCmd == SW_SHOWMINNOACTIVE)
        saved.showCmd = (saved.flags & WPF_RESTORETOMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    saved.flags &= ~(UINT)WPF_RESTORETOMAXIMIZED;
    const bool savedMax = saved.showCmd == SW_SHOWMAXIMIZED;
    switch (nCmdShow) {
    case SW_SHOWMINIMIZED: case SW_MINIMIZE: case SW_SHOWMINNOACTIVE:
        // "Run: Minimized": minimized, restoring to how it was saved.
        saved.showCmd = (UINT)nCmdShow;
        if (savedMax) saved.flags |= WPF_RESTORETOMAXIMIZED;
        break;
    case SW_SHOWMAXIMIZED:   // "Run: Maximized" (SW_MAXIMIZE is the same value)
        saved.showCmd = SW_SHOWMAXIMIZED;
        break;
    case SW_SHOWNOACTIVATE: case SW_SHOWNA:
        // Without taking the focus. A saved maximized window stays maximized, which
        // Windows can't show without activating it.
        if (!savedMax) saved.showCmd = SW_SHOWNOACTIVATE;
        break;
    default:   // SW_SHOWNORMAL, SW_SHOWDEFAULT, ...: no particular request
        break;
    }
    saved.length = sizeof(saved);
    return saved;
}
