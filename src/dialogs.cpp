#include "dialogs.h"
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

namespace dlg {

static HFONT guiFont() {
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static void runModal(HWND hwnd, HWND parent) {
    EnableWindow(parent, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    MSG msg;
    while (IsWindow(hwnd) && GetMessage(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessage(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

// ------------------------------------------------------------------ prompt text
struct PromptState {
    std::wstring* out;
    bool ok = false;
    HWND edit = nullptr;
};

static LRESULT CALLBACK PromptProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (PromptState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            wchar_t buf[512]; GetWindowTextW(st->edit, buf, 512);
            *st->out = buf; st->ok = true; DestroyWindow(h); return 0;
        } else if (LOWORD(w) == IDCANCEL) {
            st->ok = false; DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: st->ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool promptText(HWND parent, const wchar_t* title, const wchar_t* label, std::wstring& text) {
    static bool reg = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = PromptProc; wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ACE_Prompt"; RegisterClassW(&wc); reg = true;
    }
    PromptState st; st.out = &text;
    RECT pr; GetWindowRect(parent, &pr);
    int W = 380, H = 150;
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ACE_Prompt", title,
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    HWND lbl = CreateWindowW(L"STATIC", label, WS_CHILD | WS_VISIBLE,
        16, 14, 340, 20, h, nullptr, hInst, nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        16, 38, 344, 24, h, nullptr, hInst, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        176, 76, 88, 28, h, (HMENU)IDOK, hInst, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        272, 76, 88, 28, h, (HMENU)IDCANCEL, hInst, nullptr);
    for (HWND c : { lbl, st.edit, ok, cancel }) SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(st.edit, EM_SETSEL, 0, -1);
    SetFocus(st.edit);

    runModal(h, parent);
    return st.ok;
}

// ------------------------------------------------------------------ open files
std::vector<std::wstring> openAudioFiles(HWND parent) {
    std::vector<std::wstring> result;
    std::vector<wchar_t> buf(65536, 0);
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = parent;
    ofn.lpstrFilter =
        L"Audio files\0*.wav;*.mp3;*.m4a;*.aac;*.flac;*.wma;*.ogg;*.aiff;*.aif\0All files\0*.*\0";
    ofn.lpstrFile = buf.data(); ofn.nMaxFile = (DWORD)buf.size();
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return result;

    // Parse multiselect result.
    const wchar_t* p = buf.data();
    std::wstring dir = p; p += dir.size() + 1;
    if (*p == 0) { // single file selected (dir actually holds full path)
        result.push_back(dir);
    } else {
        while (*p) {
            std::wstring name = p; p += name.size() + 1;
            result.push_back(dir + L"\\" + name);
        }
    }
    return result;
}

// ------------------------------------------------------------------ export options
struct ExportState {
    mfio::ExportOptions* opts;
    std::wstring* path;
    std::wstring suggested;
    HWND cbFormat, cbRate, cbBits, cbBitrate, cbChannels;
    bool ok = false;
    HWND parent;
};

static const int kBitrates[] = { 96, 128, 160, 192, 256, 320 };
static const int kRates[]    = { 22050, 32000, 44100, 48000, 96000 };
static const int kBits[]     = { 16, 24, 32 };   // 32 = IEEE float (WAV only)

static void updateExportEnable(ExportState* st) {
    int fi = (int)SendMessageW(st->cbFormat, CB_GETCURSEL, 0, 0);
    bool isWav = (fi == 0);
    EnableWindow(st->cbBitrate, !isWav);   // bitrate only for compressed formats
    EnableWindow(st->cbBits, isWav);       // bit depth only for WAV
}

static LRESULT CALLBACK ExportProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (ExportState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (HIWORD(w) == CBN_SELCHANGE && (HWND)l == st->cbFormat) {
            updateExportEnable(st);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { st->ok = false; DestroyWindow(h); return 0; }
        if (LOWORD(w) == IDOK) {
            int fi = (int)SendMessageW(st->cbFormat, CB_GETCURSEL, 0, 0);
            int ri = (int)SendMessageW(st->cbRate, CB_GETCURSEL, 0, 0);
            int di = (int)SendMessageW(st->cbBits, CB_GETCURSEL, 0, 0);
            int bi = (int)SendMessageW(st->cbBitrate, CB_GETCURSEL, 0, 0);
            int ci = (int)SendMessageW(st->cbChannels, CB_GETCURSEL, 0, 0);
            mfio::ExportFormat fmt = (mfio::ExportFormat)fi;
            st->opts->format = fmt;
            st->opts->sampleRate = kRates[ri < 0 ? 3 : ri];
            st->opts->bitsPerSample = kBits[di < 0 ? 0 : di];
            st->opts->bitrateKbps = kBitrates[bi < 0 ? 3 : bi];
            st->opts->channels = (ci == 1) ? 1 : 2;

            // Save-file dialog with the matching extension.
            const wchar_t* ext = mfio::extensionFor(fmt);
            std::wstring name = st->suggested + ext;
            std::vector<wchar_t> buf(1024, 0);
            wcsncpy(buf.data(), name.c_str(), 1023);
            std::wstring filterStr = std::wstring(mfio::labelFor(fmt)) + L" (*" + ext + L")";
            std::vector<wchar_t> filter;
            auto app = [&](const std::wstring& s) { for (wchar_t c : s) filter.push_back(c); filter.push_back(0); };
            app(filterStr); app(std::wstring(L"*") + ext); filter.push_back(0);

            OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = h;
            ofn.lpstrFilter = filter.data();
            ofn.lpstrFile = buf.data(); ofn.nMaxFile = 1024;
            ofn.lpstrDefExt = ext + 1; // skip dot
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetSaveFileNameW(&ofn)) {
                *st->path = buf.data();
                st->ok = true;
                DestroyWindow(h);
            }
            return 0;
        }
        break;
    case WM_CLOSE: st->ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool exportOptions(HWND parent, mfio::ExportOptions& opts, std::wstring& outPath,
                   const std::wstring& suggestedName) {
    static bool reg = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = ExportProc; wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ACE_Export"; RegisterClassW(&wc); reg = true;
    }
    ExportState st; st.opts = &opts; st.path = &outPath; st.suggested = suggestedName; st.parent = parent;

    RECT pr; GetWindowRect(parent, &pr);
    int W = 360, H = 304;
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ACE_Export", L"Save As...",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    auto label = [&](const wchar_t* t, int yy) {
        HWND c = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, 16, yy, 96, 20, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    };
    auto combo = [&](int yy) {
        HWND c = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            120, yy, 216, 200, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };
    label(L"File type:", 18);     st.cbFormat = combo(16);
    label(L"Sample rate:", 54);   st.cbRate = combo(52);
    label(L"Bit depth:", 90);     st.cbBits = combo(88);
    label(L"Bitrate:", 126);      st.cbBitrate = combo(124);
    label(L"Channels:", 162);     st.cbChannels = combo(160);

    for (auto* f : { L"WAV (PCM)", L"MP3", L"AAC (.m4a)", L"Windows Media Audio" })
        SendMessageW(st.cbFormat, CB_ADDSTRING, 0, (LPARAM)f);
    for (int r : kRates) {
        std::wstring s = std::to_wstring(r) + L" Hz";
        SendMessageW(st.cbRate, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    for (auto* b : { L"16-bit", L"24-bit", L"32-bit float" })
        SendMessageW(st.cbBits, CB_ADDSTRING, 0, (LPARAM)b);
    for (int b : kBitrates) {
        std::wstring s = std::to_wstring(b) + L" kbps";
        SendMessageW(st.cbBitrate, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    SendMessageW(st.cbChannels, CB_ADDSTRING, 0, (LPARAM)L"Stereo");
    SendMessageW(st.cbChannels, CB_ADDSTRING, 0, (LPARAM)L"Mono");

    // defaults
    SendMessageW(st.cbFormat, CB_SETCURSEL, (int)opts.format, 0);
    int rIdx = 3; for (int i = 0; i < 5; ++i) if (kRates[i] == opts.sampleRate) rIdx = i;
    SendMessageW(st.cbRate, CB_SETCURSEL, rIdx, 0);
    int dIdx = 0; for (int i = 0; i < 3; ++i) if (kBits[i] == opts.bitsPerSample) dIdx = i;
    SendMessageW(st.cbBits, CB_SETCURSEL, dIdx, 0);
    int brIdx = 3; for (int i = 0; i < 6; ++i) if (kBitrates[i] == opts.bitrateKbps) brIdx = i;
    SendMessageW(st.cbBitrate, CB_SETCURSEL, brIdx, 0);
    SendMessageW(st.cbChannels, CB_SETCURSEL, opts.channels == 1 ? 1 : 0, 0);

    HWND ok = CreateWindowW(L"BUTTON", L"Save...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        150, 224, 88, 30, h, (HMENU)IDOK, hInst, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        246, 224, 88, 30, h, (HMENU)IDCANCEL, hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)guiFont(), TRUE);

    updateExportEnable(&st);
    SetFocus(st.cbFormat);
    runModal(h, parent);
    return st.ok;
}

// ------------------------------------------------------------------ voice cleaner
enum { IDC_VC_ALGO = 2001, IDC_VC_GETPROFILE = 2002 };

struct VCState {
    VoiceCleanerContext* ctx = nullptr;
    HWND cbAlgo = 0;
    HWND lbStrength = 0, cbStrength = 0;
    HWND lbProfile = 0, btnProfile = 0, stStatus = 0;
    HWND lbDb = 0, edDb = 0, lbSens = 0, edSens = 0, lbBands = 0, edBands = 0;
    HWND lbNoise = 0, rbReduce = 0, rbResidue = 0;
    bool ok = false;
};

static double vcReadDouble(HWND edit, double lo, double hi, double fallback) {
    wchar_t buf[64]{}; GetWindowTextW(edit, buf, 64);
    wchar_t* end = nullptr;
    double v = wcstod(buf, &end);
    if (end == buf) v = fallback;
    return std::max(lo, std::min(hi, v));
}

static void vcUpdateStatus(VCState* st) {
    std::wstring s;
    if (st->ctx->profile && st->ctx->profile->valid())
        s = L"Profile: " + *st->ctx->profileDesc;
    else if (st->ctx->noiseSelection)
        s = L"No profile yet \u2014 click Get Noise Profile to capture the current selection.";
    else
        s = L"No profile yet \u2014 select a noise-only span in a clip first, then reopen.";
    SetWindowTextW(st->stStatus, s.c_str());
}

static void vcUpdateVisibility(VCState* st) {
    const bool prof = SendMessageW(st->cbAlgo, CB_GETCURSEL, 0, 0) == 2;
    for (HWND c : { st->lbStrength, st->cbStrength })
        ShowWindow(c, prof ? SW_HIDE : SW_SHOW);
    for (HWND c : { st->lbProfile, st->btnProfile, st->stStatus, st->lbDb, st->edDb,
                    st->lbSens, st->edSens, st->lbBands, st->edBands,
                    st->lbNoise, st->rbReduce, st->rbResidue })
        ShowWindow(c, prof ? SW_SHOW : SW_HIDE);
}

static LRESULT CALLBACK VCProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (VCState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDC_VC_ALGO && HIWORD(w) == CBN_SELCHANGE) {
            vcUpdateVisibility(st); return 0;
        }
        if (LOWORD(w) == IDC_VC_GETPROFILE) {
            if (st->ctx->noiseSelection) {
                dsp::NoiseProfile p = dsp::computeNoiseProfile(*st->ctx->noiseSelection);
                if (!p.valid()) {
                    MessageBoxW(h,
                        L"Selected noise profile is too short.\n"
                        L"Select at least ~50 ms of noise-only audio.",
                        L"Voice Cleaner", MB_ICONINFORMATION);
                } else {
                    *st->ctx->profile = std::move(p);
                    wchar_t d[160];
                    swprintf(d, 160, L"%.2f s from %s", st->ctx->profile->seconds,
                             st->ctx->selectionDesc.c_str());
                    *st->ctx->profileDesc = d;
                    st->ctx->captured = true;   // caller adds this to recent captures
                }
                vcUpdateStatus(st);
            }
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { st->ok = false; DestroyWindow(h); return 0; }
        if (LOWORD(w) == IDOK) {
            const int ai = (int)SendMessageW(st->cbAlgo, CB_GETCURSEL, 0, 0);
            const int si = (int)SendMessageW(st->cbStrength, CB_GETCURSEL, 0, 0);
            dsp::NROptions& o = *st->ctx->opts;
            if (ai == 2 && !(st->ctx->profile && st->ctx->profile->valid())) {
                MessageBoxW(h,
                    L"No noise profile has been captured.\n\n"
                    L"Close this dialog, drag-select a span that contains only noise "
                    L"(no speech) on any clip, reopen the voice cleaner and click "
                    L"Get Noise Profile.",
                    L"Voice Cleaner", MB_ICONINFORMATION);
                return 0;
            }
            o.algorithm = ai == 2 ? dsp::NRAlgorithm::Profile
                        : ai == 1 ? dsp::NRAlgorithm::Wiener
                                  : dsp::NRAlgorithm::SpectralSubtraction;
            o.strength = si == 0 ? dsp::NRStrength::Light : si == 2 ? dsp::NRStrength::Aggressive : dsp::NRStrength::Medium;
            o.profile.reductionDb = (float)vcReadDouble(st->edDb, 0.0, 48.0, 6.0);
            o.profile.sensitivity = (float)vcReadDouble(st->edSens, 0.01, 24.0, 6.0);
            o.profile.freqSmoothingBands = (int)(vcReadDouble(st->edBands, 0.0, 12.0, 6.0) + 0.5);
            o.profile.residue = SendMessageW(st->rbResidue, BM_GETCHECK, 0, 0) == BST_CHECKED;
            st->ok = true; DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: st->ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool voiceCleaner(HWND parent, VoiceCleanerContext& ctx) {
    static bool reg = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = VCProc; wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ACE_VC"; RegisterClassW(&wc); reg = true;
    }
    dsp::NROptions& opts = *ctx.opts;
    VCState st; st.ctx = &ctx;
    RECT pr; GetWindowRect(parent, &pr);
    int W = 430, H = 368;
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ACE_VC", L"Voice Cleaner",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    HWND info = CreateWindowW(L"STATIC",
        L"Reduces steady background noise (hum, hiss, fans).\n"
        L"Auto algorithms profile quiet gaps; Noise profile uses a captured sample.",
        WS_CHILD | WS_VISIBLE, 16, 12, 396, 36, h, nullptr, hInst, nullptr);
    SendMessageW(info, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    auto label = [&](const wchar_t* t, int yy, int w = 180) {
        HWND c = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, 16, yy, w, 20, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };
    auto combo = [&](int yy, int id = 0) {
        HWND c = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            210, yy, 202, 200, h, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };
    auto edit = [&](int yy, const std::wstring& text) {
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            210, yy, 80, 22, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };

    label(L"Algorithm:", 58);
    st.cbAlgo = combo(56, IDC_VC_ALGO);
    for (auto* a : { L"Spectral subtraction", L"Wiener filter", L"Noise profile (Audacity-style)" })
        SendMessageW(st.cbAlgo, CB_ADDSTRING, 0, (LPARAM)a);

    // Auto-algorithm row (shares the row below Algorithm with the profile button)
    st.lbStrength = label(L"Strength:", 94);
    st.cbStrength = combo(92);
    for (auto* s : { L"Light", L"Medium", L"Aggressive" })
        SendMessageW(st.cbStrength, CB_ADDSTRING, 0, (LPARAM)s);

    // Profile rows
    st.lbProfile = label(L"Noise profile:", 94);
    st.btnProfile = CreateWindowW(L"BUTTON", L"Get Noise Profile",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 210, 90, 202, 26, h, (HMENU)IDC_VC_GETPROFILE, hInst, nullptr);
    SendMessageW(st.btnProfile, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    EnableWindow(st.btnProfile, ctx.noiseSelection != nullptr);
    st.stStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 122, 396, 18, h, nullptr, hInst, nullptr);
    SendMessageW(st.stStatus, WM_SETFONT, (WPARAM)guiFont(), TRUE);

    wchar_t num[64];
    swprintf(num, 64, L"%g", (double)opts.profile.reductionDb);
    st.lbDb = label(L"Noise reduction (dB):", 150);
    st.edDb = edit(148, num);
    swprintf(num, 64, L"%.2f", (double)opts.profile.sensitivity);
    st.lbSens = label(L"Sensitivity:", 182);
    st.edSens = edit(180, num);
    swprintf(num, 64, L"%d", opts.profile.freqSmoothingBands);
    st.lbBands = label(L"Frequency smoothing (bands):", 214);
    st.edBands = edit(212, num);
    st.lbNoise = label(L"Noise:", 246);
    st.rbReduce = CreateWindowW(L"BUTTON", L"Reduce",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
        210, 244, 90, 20, h, nullptr, hInst, nullptr);
    st.rbResidue = CreateWindowW(L"BUTTON", L"Residue",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        306, 244, 100, 20, h, nullptr, hInst, nullptr);
    for (HWND c : { st.rbReduce, st.rbResidue }) SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(opts.profile.residue ? st.rbResidue : st.rbReduce, BM_SETCHECK, BST_CHECKED, 0);

    const int ai = opts.algorithm == dsp::NRAlgorithm::Profile ? 2
                 : opts.algorithm == dsp::NRAlgorithm::Wiener ? 1 : 0;
    SendMessageW(st.cbAlgo, CB_SETCURSEL, ai, 0);
    int si = opts.strength == dsp::NRStrength::Light ? 0 : opts.strength == dsp::NRStrength::Aggressive ? 2 : 1;
    SendMessageW(st.cbStrength, CB_SETCURSEL, si, 0);

    HWND ok = CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        218, 286, 88, 30, h, (HMENU)IDOK, hInst, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        314, 286, 88, 30, h, (HMENU)IDCANCEL, hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)guiFont(), TRUE);

    vcUpdateStatus(&st);
    vcUpdateVisibility(&st);
    SetFocus(st.cbAlgo);
    runModal(h, parent);
    return st.ok;
}

// ------------------------------------------------------------------ voice isolation
struct VIState {
    dsp::VoiceIsolateOptions* opts = nullptr;
    HWND cbSens = 0, edDb = 0, edHold = 0, edFade = 0;
    HWND rbPreview = 0;   // checkbox: output the removed material instead (residue)
    bool ok = false;
};

// Sensitivity presets shown in the combo (kept in sync with the labels below).
static const float kVISens[3] = { 0.25f, 0.50f, 0.75f };

static LRESULT CALLBACK VIProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (VIState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDCANCEL) { st->ok = false; DestroyWindow(h); return 0; }
        if (LOWORD(w) == IDOK) {
            dsp::VoiceIsolateOptions& o = *st->opts;
            int si = (int)SendMessageW(st->cbSens, CB_GETCURSEL, 0, 0);
            if (si < 0 || si > 2) si = 1;
            o.sensitivity = kVISens[si];
            o.reductionDb = (float)vcReadDouble(st->edDb, 0.0, 96.0, 60.0);
            o.holdMs      = (float)vcReadDouble(st->edHold, 0.0, 2000.0, 200.0);
            o.fadeMs      = (float)vcReadDouble(st->edFade, 0.0, 500.0, 25.0);
            o.residue     = SendMessageW(st->rbPreview, BM_GETCHECK, 0, 0) == BST_CHECKED;
            st->ok = true; DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: st->ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool voiceIsolate(HWND parent, dsp::VoiceIsolateOptions& opts, const std::wstring& scopeLabel) {
    static bool reg = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = VIProc; wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ACE_VI"; RegisterClassW(&wc); reg = true;
    }
    VIState st; st.opts = &opts;
    RECT pr; GetWindowRect(parent, &pr);
    int W = 430, H = 336;
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ACE_VI", L"Remove Non-Voice",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    HWND info = CreateWindowW(L"STATIC",
        L"Silences everything that isn't speech \u2014 bumps, shuffling, clicks, door\n"
        L"slams and the room tone between sentences. The clip's length and timing\n"
        L"are unchanged; non-voice is attenuated in place, not cut out.",
        WS_CHILD | WS_VISIBLE, 16, 12, 396, 50, h, nullptr, hInst, nullptr);
    SendMessageW(info, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    HWND scope = CreateWindowW(L"STATIC", (L"Applies to: " + scopeLabel).c_str(),
        WS_CHILD | WS_VISIBLE, 16, 64, 396, 18, h, nullptr, hInst, nullptr);
    SendMessageW(scope, WM_SETFONT, (WPARAM)guiFont(), TRUE);

    auto label = [&](const wchar_t* t, int yy) {
        HWND c = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, 16, yy, 190, 20, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };
    auto edit = [&](int yy, const std::wstring& text) {
        HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            210, yy, 80, 22, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };

    label(L"Sensitivity:", 92);
    st.cbSens = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        210, 90, 202, 200, h, nullptr, hInst, nullptr);
    SendMessageW(st.cbSens, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    for (auto* s : { L"Gentle (keep more)", L"Balanced", L"Strict (remove more)" })
        SendMessageW(st.cbSens, CB_ADDSTRING, 0, (LPARAM)s);
    int si = 1;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(opts.sensitivity - kVISens[i]) < std::fabs(opts.sensitivity - kVISens[si])) si = i;
    SendMessageW(st.cbSens, CB_SETCURSEL, si, 0);

    wchar_t num[64];
    swprintf(num, 64, L"%g", (double)opts.reductionDb);
    label(L"Attenuation (dB):", 126);      st.edDb   = edit(124, num);
    swprintf(num, 64, L"%g", (double)opts.holdMs);
    label(L"Hold after speech (ms):", 158); st.edHold = edit(156, num);
    swprintf(num, 64, L"%g", (double)opts.fadeMs);
    label(L"Fade (ms):", 190);              st.edFade = edit(188, num);

    label(L"Output:", 222);
    st.rbPreview = CreateWindowW(L"BUTTON", L"Preview what would be removed",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        210, 220, 202, 20, h, nullptr, hInst, nullptr);
    SendMessageW(st.rbPreview, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(st.rbPreview, BM_SETCHECK, opts.residue ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND ok = CreateWindowW(L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        218, 254, 88, 30, h, (HMENU)IDOK, hInst, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        314, 254, 88, 30, h, (HMENU)IDCANCEL, hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)guiFont(), TRUE);

    SetFocus(st.cbSens);
    runModal(h, parent);
    return st.ok;
}

// ------------------------------------------------------------------ project files
std::wstring openProject(HWND parent) {
    wchar_t buf[1024] = { 0 };
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"Audio Clip Editor project (*.acep)\0*.acep\0All files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = 1024;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) return buf;
    return L"";
}

std::wstring saveProject(HWND parent, const std::wstring& suggested) {
    wchar_t buf[1024] = { 0 };
    std::wstring s = suggested + L".acep";
    wcsncpy(buf, s.c_str(), 1023);
    OPENFILENAMEW ofn{}; ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"Audio Clip Editor project (*.acep)\0*.acep\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = 1024;
    ofn.lpstrDefExt = L"acep";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) return buf;
    return L"";
}

} // namespace dlg
