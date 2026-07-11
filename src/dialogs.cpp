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
    HWND cbFormat, cbBitrate, cbChannels;
    bool ok = false;
    HWND parent;
};

static const int kBitrates[] = { 96, 128, 160, 192, 256, 320 };

static void updateBitrateEnable(ExportState* st) {
    int fi = (int)SendMessageW(st->cbFormat, CB_GETCURSEL, 0, 0);
    bool isWav = (fi == 0);
    EnableWindow(st->cbBitrate, !isWav);
}

static LRESULT CALLBACK ExportProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (ExportState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_COMMAND:
        if (HIWORD(w) == CBN_SELCHANGE && (HWND)l == st->cbFormat) {
            updateBitrateEnable(st);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) { st->ok = false; DestroyWindow(h); return 0; }
        if (LOWORD(w) == IDOK) {
            int fi = (int)SendMessageW(st->cbFormat, CB_GETCURSEL, 0, 0);
            int bi = (int)SendMessageW(st->cbBitrate, CB_GETCURSEL, 0, 0);
            int ci = (int)SendMessageW(st->cbChannels, CB_GETCURSEL, 0, 0);
            mfio::ExportFormat fmt = (mfio::ExportFormat)fi;
            st->opts->format = fmt;
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
    int W = 360, H = 220;
    int x = pr.left + ((pr.right - pr.left) - W) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - H) / 2;
    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ACE_Export", L"Save As...",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H, parent, nullptr, hInst, nullptr);
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&st);

    auto label = [&](const wchar_t* t, int yy) {
        HWND c = CreateWindowW(L"STATIC", t, WS_CHILD | WS_VISIBLE, 16, yy, 90, 20, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    };
    auto combo = [&](int yy) {
        HWND c = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            116, yy, 220, 200, h, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)guiFont(), TRUE);
        return c;
    };
    label(L"File type:", 18);   st.cbFormat = combo(16);
    label(L"Bitrate:", 54);     st.cbBitrate = combo(52);
    label(L"Channels:", 90);    st.cbChannels = combo(88);

    for (auto* f : { L"WAV (PCM 16-bit)", L"MP3", L"AAC (.m4a)", L"Windows Media Audio" })
        SendMessageW(st.cbFormat, CB_ADDSTRING, 0, (LPARAM)f);
    for (int b : kBitrates) {
        std::wstring s = std::to_wstring(b) + L" kbps";
        SendMessageW(st.cbBitrate, CB_ADDSTRING, 0, (LPARAM)s.c_str());
    }
    SendMessageW(st.cbChannels, CB_ADDSTRING, 0, (LPARAM)L"Stereo");
    SendMessageW(st.cbChannels, CB_ADDSTRING, 0, (LPARAM)L"Mono");

    // defaults
    int fmtIdx = (int)opts.format;
    SendMessageW(st.cbFormat, CB_SETCURSEL, fmtIdx, 0);
    int brIdx = 3; for (int i = 0; i < 6; ++i) if (kBitrates[i] == opts.bitrateKbps) brIdx = i;
    SendMessageW(st.cbBitrate, CB_SETCURSEL, brIdx, 0);
    SendMessageW(st.cbChannels, CB_SETCURSEL, opts.channels == 1 ? 1 : 0, 0);

    HWND ok = CreateWindowW(L"BUTTON", L"Save...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        150, 140, 88, 30, h, (HMENU)IDOK, hInst, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        246, 140, 88, 30, h, (HMENU)IDCANCEL, hInst, nullptr);
    SendMessageW(ok, WM_SETFONT, (WPARAM)guiFont(), TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)guiFont(), TRUE);

    updateBitrateEnable(&st);
    SetFocus(st.cbFormat);
    runModal(h, parent);
    return st.ok;
}

} // namespace dlg
