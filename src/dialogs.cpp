#include "dialogs.h"
#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

namespace dlg {

// ------------------------------------------------------------------ dialog layout
// These dialogs are built in code rather than from .rc resources, so they have to
// do what the dialog manager would otherwise do for them: take the system UI font
// at the target monitor's DPI, scale every coordinate by that DPI, and size things
// from *measured* text instead of from constants that only happen to fit at 100%
// scaling with one particular font. (Hardcoding both is what made the Remove
// Non-Voice dialog clip its wrapped intro paragraph and its checkbox label.)
// DlgUI is that scaffolding; every dialog below lays itself out through it.
struct DlgUI {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HWND dlg = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    bool ownFont = false;
    int lineH = 16;                       // one line of the UI font
    int margin = 16, gap = 12, rowGap = 10;   // scaled in the constructor

    explicit DlgUI(HWND parent) {
        const UINT d = parent ? GetDpiForWindow(parent) : 0;
        dpi = d ? d : 96;
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
            font = CreateFontIndirectW(&ncm.lfMessageFont);
            ownFont = font != nullptr;
        }
        if (!font) font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        lineH = measure(L"Ag").cy;
        margin = S(16); gap = S(12); rowGap = S(10);
    }
    ~DlgUI() { if (ownFont) DeleteObject(font); }
    DlgUI(const DlgUI&) = delete;
    DlgUI& operator=(const DlgUI&) = delete;

    int S(int v) const { return MulDiv(v, (int)dpi, 96); }
    int rowH()  const { return std::max(S(22), lineH + S(6)); }   // combo / checkbox row
    int editH() const { return std::max(S(22), lineH + S(8)); }   // edit box (has a border)
    int btnH()  const { return std::max(S(28), lineH + S(12)); }

    // Measured text size. wrapWidth > 0 word-wraps to that width (multi-line).
    SIZE measure(const wchar_t* text, int wrapWidth = 0) const {
        HDC dc = GetDC(nullptr);
        HFONT old = (HFONT)SelectObject(dc, font);
        RECT r{ 0, 0, wrapWidth > 0 ? wrapWidth : 0, 0 };
        DrawTextW(dc, text, -1, &r, DT_CALCRECT | DT_NOPREFIX |
                                    (wrapWidth > 0 ? DT_WORDBREAK : DT_SINGLELINE));
        SelectObject(dc, old); ReleaseDC(nullptr, dc);
        return SIZE{ r.right - r.left, r.bottom - r.top };
    }
    int textW(const wchar_t* t) const { return measure(t).cx; }
    int btnW(const wchar_t* t) const { return std::max(S(84), textW(t) + S(28)); }
    int checkW(const wchar_t* t) const { return textW(t) + S(26); }   // + box and spacing
    int comboW(const wchar_t* t) const { return textW(t) + S(38); }   // + drop-down arrow

    // Create the dialog window sized to hold `clientW` x `clientH` of content,
    // centred on the parent but kept inside its monitor's work area.
    HWND create(const wchar_t* cls, const wchar_t* title, HWND parent, int clientW, int clientH) {
        const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
        RECT r{ 0, 0, clientW, clientH };
        AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
        const int W = r.right - r.left, H = r.bottom - r.top;
        RECT pr{}; GetWindowRect(parent, &pr);
        int x = pr.left + ((pr.right - pr.left) - W) / 2;
        int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi)) {
            x = std::max((int)mi.rcWork.left, std::min(x, (int)mi.rcWork.right - W));
            y = std::max((int)mi.rcWork.top, std::min(y, (int)mi.rcWork.bottom - H));
        }
        dlg = CreateWindowExW(ex, cls, title, style, x, y, W, H, parent, nullptr, hInst, nullptr);
        return dlg;
    }

    HWND ctl(const wchar_t* cls, const wchar_t* text, DWORD style,
             int x, int y, int w, int h, int id = 0, DWORD ex = 0) const {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 x, y, w, h, dlg, (HMENU)(INT_PTR)id, hInst, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    }
    HWND label(const wchar_t* t, int x, int y, int w, int h, DWORD style = 0) const {
        return ctl(L"STATIC", t, style, x, y, w, h);
    }
    // A label vertically centred against a control of height `h` on the same row.
    HWND rowLabel(const wchar_t* t, int x, int y, int w, int h) const {
        return label(t, x, y + (h - lineH) / 2, w, lineH);
    }
    HWND edit(const wchar_t* t, int x, int y, int w) const {
        return ctl(L"EDIT", t, WS_TABSTOP | ES_AUTOHSCROLL, x, y, w, editH(), 0, WS_EX_CLIENTEDGE);
    }
    HWND combo(int x, int y, int w, int items, int id = 0) const {
        return ctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                   x, y, w, rowH() + items * (lineH + S(4)), id);
    }
    HWND button(const wchar_t* t, int x, int y, int w, int h, int id, bool def = false) const {
        return ctl(L"BUTTON", t, WS_TABSTOP | (def ? BS_DEFPUSHBUTTON : 0), x, y, w, h, id);
    }
    HWND check(const wchar_t* t, int x, int y, int w) const {
        return ctl(L"BUTTON", t, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, rowH());
    }
    HWND radio(const wchar_t* t, int x, int y, int w, bool group) const {
        return ctl(L"BUTTON", t, WS_TABSTOP | BS_AUTORADIOBUTTON | (group ? WS_GROUP : 0),
                   x, y, w, rowH());
    }
};

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

// ------------------------------------------------------------------ "Range:" rows
// Shared by the voice cleaner and remove-non-voice dialogs: lets the user run the
// effect over just the current waveform selection instead of the whole clip. Two
// stacked radios rather than one checkbox, so the default is explicit and a long
// selection description can't squeeze the labels. The rows are omitted entirely
// when the operation targets more than one clip (no single selection applies).
struct RangeRows {
    RangeOption* opt = nullptr;
    HWND rbWhole = 0, rbSel = 0;
    std::wstring wholeText, selText;

    explicit RangeRows(RangeOption& o) : opt(&o) {
        wholeText = L"The whole clip";
        selText = o.hasSelection ? L"Only the selection (" + o.selectionDesc + L")"
                                 : L"Only the selection \u2014 nothing is selected";
    }
    bool shown() const { return opt->offer; }
    static const wchar_t* rowLabelText() { return L"Range:"; }

    // Width the radio column needs, and the height the rows add, so the dialog
    // can size itself before anything is created.
    int width(const DlgUI& ui) const {
        if (!shown()) return 0;
        return std::max(ui.checkW(wholeText.c_str()), ui.checkW(selText.c_str()));
    }
    // The two radios belong together, so they sit a hair apart rather than a
    // full row apart; the normal row gap follows the pair.
    int innerGap(const DlgUI& ui) const { return ui.S(2); }
    int height(const DlgUI& ui, int rowH, int step) const {
        return shown() ? rowH + innerGap(ui) + step : 0;
    }

    void build(const DlgUI& ui, int& y, int labelW, int ctlW, int rowH, int step) {
        if (!shown()) return;
        ui.rowLabel(rowLabelText(), ui.margin, y, labelW, rowH);
        rbWhole = ui.radio(wholeText.c_str(), ui.margin + labelW + ui.gap, y, ctlW, true);
        y += rowH + innerGap(ui);
        rbSel = ui.radio(selText.c_str(), ui.margin + labelW + ui.gap, y, ctlW, false);
        y += step;
        EnableWindow(rbSel, opt->hasSelection);
        const bool sel = opt->hasSelection && opt->selectionOnly;
        SendMessageW(sel ? rbSel : rbWhole, BM_SETCHECK, BST_CHECKED, 0);
    }
    void commit() const {
        opt->selectionOnly = shown() && rbSel &&
                             SendMessageW(rbSel, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
};

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
    DlgUI ui(parent);

    const int contentW = std::max(ui.S(330), ui.textW(label));
    const int bw = ui.btnW(L"Cancel"), bh = ui.btnH();
    const int clientH = ui.margin + ui.lineH + ui.S(6) + ui.editH() + ui.S(18) + bh + ui.margin;
    HWND h = ui.create(L"ACE_Prompt", title, parent, ui.margin * 2 + contentW, clientH);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    int y = ui.margin;
    ui.label(label, ui.margin, y, contentW, ui.lineH);  y += ui.lineH + ui.S(6);
    st.edit = ui.edit(text.c_str(), ui.margin, y, contentW);
    y += ui.editH() + ui.S(18);
    const int right = ui.margin + contentW;
    ui.button(L"OK", right - bw * 2 - ui.S(8), y, bw, bh, IDOK, true);
    ui.button(L"Cancel", right - bw, y, bw, bh, IDCANCEL);
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
    DlgUI ui(parent);

    static const wchar_t* kRowLabels[] = { L"File type:", L"Sample rate:", L"Bit depth:",
                                           L"Bitrate:", L"Channels:" };
    int labelW = 0;
    for (auto* t : kRowLabels) labelW = std::max(labelW, ui.textW(t));
    int ctlW = ui.S(150);
    for (auto* t : { L"WAV (PCM)", L"MP3", L"AAC (.m4a)", L"Windows Media Audio",
                     L"32-bit float", L"192000 Hz", L"320 kbps", L"Stereo" })
        ctlW = std::max(ctlW, ui.comboW(t));
    const int contentW = labelW + ui.gap + ctlW;
    const int ctlX = ui.margin + labelW + ui.gap;
    const int rowH = ui.rowH(), step = rowH + ui.rowGap;
    const int bw = ui.btnW(L"Save..."), bh = ui.btnH();
    const int clientH = ui.margin + 5 * step + ui.S(8) + bh + ui.margin;

    HWND h = ui.create(L"ACE_Export", L"Save As...", parent, ui.margin * 2 + contentW, clientH);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    int y = ui.margin;
    auto row = [&](const wchar_t* t) {
        ui.rowLabel(t, ui.margin, y, labelW, rowH);
        HWND c = ui.combo(ctlX, y, ctlW, 8);
        y += step;
        return c;
    };
    st.cbFormat   = row(kRowLabels[0]);
    st.cbRate     = row(kRowLabels[1]);
    st.cbBits     = row(kRowLabels[2]);
    st.cbBitrate  = row(kRowLabels[3]);
    st.cbChannels = row(kRowLabels[4]);

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

    y += ui.S(8);
    const int right = ui.margin + contentW;
    ui.button(L"Save...", right - bw * 2 - ui.S(8), y, bw, bh, IDOK, true);
    ui.button(L"Cancel", right - bw, y, bw, bh, IDCANCEL);

    updateExportEnable(&st);
    SetFocus(st.cbFormat);
    runModal(h, parent);
    return st.ok;
}

// ------------------------------------------------------------------ voice cleaner
enum { IDC_VC_ALGO = 2001, IDC_VC_GETPROFILE = 2002 };

struct VCState {
    VoiceCleanerContext* ctx = nullptr;
    RangeRows* range = nullptr;
    HWND cbAlgo = 0;
    HWND lbStrength = 0, cbStrength = 0;
    HWND lbProfile = 0, btnProfile = 0, stStatus = 0;
    HWND lbDb = 0, edDb = 0, lbSens = 0, edSens = 0, lbBands = 0, edBands = 0;
    HWND lbNoise = 0, rbReduce = 0, rbResidue = 0;
    HWND btnOk = 0, btnCancel = 0;
    // The profile algorithm needs five extra rows, so the dialog grows/shrinks with
    // the chosen algorithm instead of leaving a hole where the hidden rows were.
    UINT dpi = 96;
    int clientW = 0, autoH = 0, profH = 0;   // client size per mode
    int autoBtnY = 0, profBtnY = 0;          // button row per mode
    int btnX = 0, btnW = 0, btnH = 0, btnGap = 0;
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

static void vcUpdateVisibility(VCState* st, HWND dlg) {
    const bool prof = SendMessageW(st->cbAlgo, CB_GETCURSEL, 0, 0) == 2;
    for (HWND c : { st->lbStrength, st->cbStrength })
        ShowWindow(c, prof ? SW_HIDE : SW_SHOW);
    for (HWND c : { st->lbProfile, st->btnProfile, st->stStatus, st->lbDb, st->edDb,
                    st->lbSens, st->edSens, st->lbBands, st->edBands,
                    st->lbNoise, st->rbReduce, st->rbResidue })
        ShowWindow(c, prof ? SW_SHOW : SW_HIDE);

    // Follow the visible rows: move the buttons up and shrink the frame.
    const int by = prof ? st->profBtnY : st->autoBtnY;
    SetWindowPos(st->btnOk, nullptr, st->btnX, by, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    SetWindowPos(st->btnCancel, nullptr, st->btnX + st->btnW + st->btnGap, by, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER);
    RECT r{ 0, 0, st->clientW, prof ? st->profH : st->autoH };
    AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(dlg, GWL_STYLE), FALSE,
                             (DWORD)GetWindowLongPtrW(dlg, GWL_EXSTYLE), st->dpi);
    SetWindowPos(dlg, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    InvalidateRect(dlg, nullptr, TRUE);
}

static LRESULT CALLBACK VCProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (VCState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDC_VC_ALGO && HIWORD(w) == CBN_SELCHANGE) {
            vcUpdateVisibility(st, h); return 0;
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
            st->range->commit();
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
    RangeRows range(ctx.range);
    VCState st; st.ctx = &ctx; st.range = &range;
    DlgUI ui(parent);

    static const wchar_t* kInfo =
        L"Reduces steady background noise (hum, hiss, fans). Auto algorithms profile "
        L"quiet gaps; Noise profile uses a captured sample.";
    static const wchar_t* kLongStatus =
        L"No profile yet \u2014 select a noise-only span in a clip first, then reopen.";
    static const wchar_t* kLabels[] = { L"Algorithm:", L"Strength:", L"Noise profile:",
                                        L"Noise reduction (dB):", L"Sensitivity:",
                                        L"Frequency smoothing (bands):", L"Noise:" };
    static const wchar_t* kAlgos[] = { L"Spectral subtraction", L"Wiener filter",
                                       L"Noise profile (Audacity-style)" };

    int labelW = ui.textW(RangeRows::rowLabelText());
    for (auto* t : kLabels) labelW = std::max(labelW, ui.textW(t));
    int ctlW = ui.S(150);
    for (auto* t : kAlgos) ctlW = std::max(ctlW, ui.comboW(t));
    ctlW = std::max(ctlW, ui.btnW(L"Get Noise Profile"));
    const int radReduce = ui.checkW(L"Reduce"), radResidue = ui.checkW(L"Residue");
    ctlW = std::max(ctlW, radReduce + ui.gap + radResidue);
    ctlW = std::max(ctlW, range.width(ui));
    const int contentW = labelW + ui.gap + ctlW;
    const int ctlX = ui.margin + labelW + ui.gap;
    const int editW = std::max(ui.S(80), ui.textW(L"000000") + ui.S(16));
    const int rowH = ui.rowH(), step = rowH + ui.rowGap;
    const int bw = ui.btnW(L"Cancel"), bh = ui.btnH();
    const SIZE infoSz = ui.measure(kInfo, contentW);
    const SIZE statusSz = ui.measure(kLongStatus, contentW);

    // Rows: info, optional Range radios, Algorithm, Strength/Noise profile
    // (shared), then — profile mode only — status, three edits and the Noise
    // radios, then the buttons.
    const int rowsTop = ui.margin + infoSz.cy + ui.S(10) + range.height(ui, rowH, step) + 2 * step;
    const int profRows = statusSz.cy + ui.S(8) + 4 * step;
    st.dpi = ui.dpi;
    st.clientW = ui.margin * 2 + contentW;
    st.autoBtnY = rowsTop + ui.S(8);
    st.profBtnY = rowsTop + profRows + ui.S(8);
    st.autoH = st.autoBtnY + bh + ui.margin;
    st.profH = st.profBtnY + bh + ui.margin;
    st.btnW = bw; st.btnH = bh; st.btnGap = ui.S(8);
    st.btnX = ui.margin + contentW - bw * 2 - st.btnGap;
    HWND h = ui.create(L"ACE_VC", L"Voice Cleaner", parent, st.clientW, st.profH);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    int y = ui.margin;
    ui.label(kInfo, ui.margin, y, contentW, infoSz.cy);  y += infoSz.cy + ui.S(10);

    range.build(ui, y, labelW, ctlW, rowH, step);

    ui.rowLabel(kLabels[0], ui.margin, y, labelW, rowH);
    st.cbAlgo = ui.combo(ctlX, y, ctlW, 4, IDC_VC_ALGO);
    for (auto* a : kAlgos) SendMessageW(st.cbAlgo, CB_ADDSTRING, 0, (LPARAM)a);
    y += step;

    // The auto-algorithm row and the profile row share this slot (only one shows).
    st.lbStrength = ui.rowLabel(kLabels[1], ui.margin, y, labelW, rowH);
    st.cbStrength = ui.combo(ctlX, y, ctlW, 4);
    for (auto* s : { L"Light", L"Medium", L"Aggressive" })
        SendMessageW(st.cbStrength, CB_ADDSTRING, 0, (LPARAM)s);
    st.lbProfile = ui.rowLabel(kLabels[2], ui.margin, y, labelW, rowH);
    st.btnProfile = ui.button(L"Get Noise Profile", ctlX, y, ctlW, rowH, IDC_VC_GETPROFILE);
    EnableWindow(st.btnProfile, ctx.noiseSelection != nullptr);
    y += step;

    st.stStatus = ui.label(L"", ui.margin, y, contentW, statusSz.cy);
    y += statusSz.cy + ui.S(8);

    wchar_t num[64];
    auto editRow = [&](const wchar_t* t, const wchar_t* value, HWND& lb, HWND& ed) {
        lb = ui.rowLabel(t, ui.margin, y, labelW, ui.editH());
        ed = ui.edit(value, ctlX, y, editW);
        y += step;
    };
    swprintf(num, 64, L"%g", (double)opts.profile.reductionDb);
    editRow(kLabels[3], num, st.lbDb, st.edDb);
    swprintf(num, 64, L"%.2f", (double)opts.profile.sensitivity);
    editRow(kLabels[4], num, st.lbSens, st.edSens);
    swprintf(num, 64, L"%d", opts.profile.freqSmoothingBands);
    editRow(kLabels[5], num, st.lbBands, st.edBands);

    st.lbNoise = ui.rowLabel(kLabels[6], ui.margin, y, labelW, rowH);
    st.rbReduce  = ui.radio(L"Reduce", ctlX, y, radReduce, true);
    st.rbResidue = ui.radio(L"Residue", ctlX + radReduce + ui.gap, y, radResidue, false);
    SendMessageW(opts.profile.residue ? st.rbResidue : st.rbReduce, BM_SETCHECK, BST_CHECKED, 0);
    y += step;

    const int ai = opts.algorithm == dsp::NRAlgorithm::Profile ? 2
                 : opts.algorithm == dsp::NRAlgorithm::Wiener ? 1 : 0;
    SendMessageW(st.cbAlgo, CB_SETCURSEL, ai, 0);
    int si = opts.strength == dsp::NRStrength::Light ? 0 : opts.strength == dsp::NRStrength::Aggressive ? 2 : 1;
    SendMessageW(st.cbStrength, CB_SETCURSEL, si, 0);

    // Placed at the profile-mode position; vcUpdateVisibility moves them per mode.
    st.btnOk = ui.button(L"Apply", st.btnX, st.profBtnY, bw, bh, IDOK, true);
    st.btnCancel = ui.button(L"Cancel", st.btnX + bw + st.btnGap, st.profBtnY, bw, bh, IDCANCEL);

    vcUpdateStatus(&st);
    vcUpdateVisibility(&st, h);
    SetFocus(st.cbAlgo);
    runModal(h, parent);
    return st.ok;
}

// ------------------------------------------------------------------ voice isolation
struct VIState {
    dsp::VoiceIsolateOptions* opts = nullptr;
    RangeRows* range = nullptr;
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
            st->range->commit();
            st->ok = true; DestroyWindow(h); return 0;
        }
        break;
    case WM_CLOSE: st->ok = false; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool voiceIsolate(HWND parent, VoiceIsolateContext& ctx) {
    static bool reg = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = VIProc; wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ACE_VI"; RegisterClassW(&wc); reg = true;
    }
    dsp::VoiceIsolateOptions& opts = *ctx.opts;
    RangeRows range(ctx.range);
    VIState st; st.opts = &opts; st.range = &range;
    DlgUI ui(parent);

    static const wchar_t* kInfo =
        L"Silences everything that isn't speech \u2014 bumps, shuffling, clicks, door slams "
        L"and the room tone between sentences. The clip's length and timing are unchanged; "
        L"non-voice is attenuated in place, not cut out.";
    static const wchar_t* kLabels[] = { L"Sensitivity:", L"Attenuation (dB):",
                                        L"Hold after speech (ms):", L"Fade (ms):", L"Output:" };
    static const wchar_t* kSens[] = { L"Gentle (keep more)", L"Balanced", L"Strict (remove more)" };
    static const wchar_t* kCheck = L"Preview what would be removed";

    int labelW = ui.textW(RangeRows::rowLabelText());
    for (auto* t : kLabels) labelW = std::max(labelW, ui.textW(t));
    int ctlW = ui.S(150);
    for (auto* t : kSens) ctlW = std::max(ctlW, ui.comboW(t));
    ctlW = std::max(ctlW, ui.checkW(kCheck));
    ctlW = std::max(ctlW, range.width(ui));
    const int contentW = labelW + ui.gap + ctlW;
    const int ctlX = ui.margin + labelW + ui.gap;
    const int editW = std::max(ui.S(80), ui.textW(L"000000") + ui.S(16));
    const int rowH = ui.rowH(), step = rowH + ui.rowGap;
    const int bw = ui.btnW(L"Cancel"), bh = ui.btnH();
    const SIZE infoSz = ui.measure(kInfo, contentW);

    // Rows: info paragraph, "Applies to", optional Range radios, sensitivity,
    // three edits, output, buttons.
    const int clientH = ui.margin + infoSz.cy + ui.S(10) + ui.lineH + ui.S(14)
                      + range.height(ui, rowH, step) + 5 * step + ui.S(8) + bh + ui.margin;
    HWND h = ui.create(L"ACE_VI", L"Remove Non-Voice", parent, ui.margin * 2 + contentW, clientH);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    int y = ui.margin;
    ui.label(kInfo, ui.margin, y, contentW, infoSz.cy);   y += infoSz.cy + ui.S(10);
    // A long clip name is ellipsised rather than allowed to wrap out of its row.
    ui.label((L"Applies to: " + ctx.scopeLabel).c_str(), ui.margin, y, contentW, ui.lineH,
             SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
    y += ui.lineH + ui.S(14);

    range.build(ui, y, labelW, ctlW, rowH, step);

    ui.rowLabel(kLabels[0], ui.margin, y, labelW, rowH);
    st.cbSens = ui.combo(ctlX, y, ctlW, 4);
    for (auto* s : kSens) SendMessageW(st.cbSens, CB_ADDSTRING, 0, (LPARAM)s);
    int si = 1;
    for (int i = 0; i < 3; ++i)
        if (std::fabs(opts.sensitivity - kVISens[i]) < std::fabs(opts.sensitivity - kVISens[si])) si = i;
    SendMessageW(st.cbSens, CB_SETCURSEL, si, 0);
    y += step;

    wchar_t num[64];
    auto editRow = [&](const wchar_t* t, const wchar_t* value) {
        ui.rowLabel(t, ui.margin, y, labelW, ui.editH());
        HWND c = ui.edit(value, ctlX, y, editW);
        y += step;
        return c;
    };
    swprintf(num, 64, L"%g", (double)opts.reductionDb);
    st.edDb = editRow(kLabels[1], num);
    swprintf(num, 64, L"%g", (double)opts.holdMs);
    st.edHold = editRow(kLabels[2], num);
    swprintf(num, 64, L"%g", (double)opts.fadeMs);
    st.edFade = editRow(kLabels[3], num);

    ui.rowLabel(kLabels[4], ui.margin, y, labelW, rowH);
    st.rbPreview = ui.check(kCheck, ctlX, y, ctlW);
    SendMessageW(st.rbPreview, BM_SETCHECK, opts.residue ? BST_CHECKED : BST_UNCHECKED, 0);
    y += step + ui.S(8);

    const int right = ui.margin + contentW;
    ui.button(L"Apply", right - bw * 2 - ui.S(8), y, bw, bh, IDOK, true);
    ui.button(L"Cancel", right - bw, y, bw, bh, IDCANCEL);

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
