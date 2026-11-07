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
// thing either effect dialog has to fit -- or, for the two matchers (5 = timbre,
// 6 = volume), the one-clip-to-the-rest wording and its longer "average of the
// other N clips on ..." reference item.
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
    } else if (which == 5 || which == 6) {
        dlg::MatchReference ref;
        ref.averageLabel = withSelection ? L"The average of the other 7 clips on 'Narration'"
                                         : L"The average of them all";
        ref.clipNames = { L"consciousness p1s1", L"consciousness p1s2",
                          L"a take with a rather long name that goes on and on", L"p2s1" };
        ref.clipIds = { 1, 2, 3, 4 };
        ref.choice = 1;
        const std::wstring matchScope =
            withSelection ? L"'consciousness p1s3' only, matched to the other 7 clips on 'Narration'"
                          : L"track 'Narration' (8 clips)";
        // A tick-list of three tracks; ticking changes only the scope line here.
        dlg::MatchTracks tracks;
        tracks.labels = { L"Narration (8 clips)", L"Pickups (3 clips)", L"Music bed (1 clip)" };
        tracks.ticked = { 1, 0, 0 };
        tracks.rebuild = [ref](const std::vector<uint8_t>& t, dlg::MatchReference& r, std::wstring& s) {
            r = ref;
            int n = 0; for (uint8_t v : t) n += v;
            s = std::to_wstring(n) + L" ticked track(s)";
            return n > 0;
        };
        if (which == 5) {
            dsp::TimbreMatchOptions o;
            bool alsoVolume = true;
            dlg::TimbreMatchContext ctx;
            ctx.opts = &o; ctx.alsoVolume = &alsoVolume;
            ctx.ref = ref; ctx.tracks = tracks; ctx.scopeLabel = matchScope;
            dlg::timbreMatch(p, ctx);
        } else {
            dlg::VolumeMatchContext ctx;
            ctx.ref = ref; ctx.tracks = tracks; ctx.scopeLabel = matchScope;
            dlg::volumeMatch(p, ctx);
        }
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
