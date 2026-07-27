#include "app.h"
#include "dialogs.h"
#include <cwchar>
#include <cstdlib>

int runSelfTest();  // selftest.cpp

// Dialog-layout harness (--dialogtest N): shows one modal dialog over a dummy
// parent so its layout can be eyeballed / screenshotted at any DPI without
// loading a project and navigating to it. These dialogs are built in code, so
// their layout is only as good as the measurements in DlgUI — being able to
// bring one up in isolation is how that gets checked. Append "sel" to exercise
// the long "Applies to: the selection in ..." scope label, which is the widest
// thing either effect dialog has to fit.
static int runDialogTest(HINSTANCE hInst, int which, bool withSelection) {
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpszClassName = L"ACE_DlgTestParent";
    RegisterClassW(&wc);
    HWND p = CreateWindowExW(0, L"ACE_DlgTestParent", L"dialog test", WS_OVERLAPPEDWINDOW,
                             80, 80, 1400, 900, nullptr, nullptr, hInst, nullptr);
    ShowWindow(p, SW_SHOW);

    const std::wstring scope =
        withSelection ? L"the selection in 'consciousness p1s1' (0:12.30 \u2013 0:18.00, 5.70 s)"
                      : L"'consciousness p1s1'";

    if (which == 1) {
        dsp::VoiceIsolateOptions o; o.sensitivity = 0.75f;
        dlg::VoiceIsolateContext ctx;
        ctx.opts = &o; ctx.scopeLabel = scope;
        dlg::voiceIsolate(p, ctx);
    } else if (which == 2) {
        dsp::NROptions o; dsp::NoiseProfile prof; std::wstring desc;
        dlg::VoiceCleanerContext ctx;
        ctx.opts = &o; ctx.profile = &prof; ctx.profileDesc = &desc; ctx.scopeLabel = scope;
        dlg::voiceCleaner(p, ctx);
    } else if (which == 3) {
        mfio::ExportOptions o; std::wstring path;
        dlg::exportOptions(p, o, path, L"clip");
    } else {
        std::wstring t = L"consciousness p1s1";
        dlg::promptText(p, L"Rename clip", L"Clip name:", t);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    if (cmdLine && wcsstr(cmdLine, L"--selftest"))
        return runSelfTest();
    if (const wchar_t* d = cmdLine ? wcsstr(cmdLine, L"--dialogtest") : nullptr)
        return runDialogTest(hInst, _wtoi(d + 12), wcsstr(d, L"sel") != nullptr);
    return runApp(hInst, nCmdShow);
}
