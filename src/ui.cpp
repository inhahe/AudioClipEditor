#include "app.h"
#include "document.h"
#include "engine.h"
#include "decoder.h"
#include "encoder.h"
#include "waveform.h"
#include "dialogs.h"
#include "dsp.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <algorithm>

#define WM_APP_PLAYEND (WM_APP + 1)

// ----------------------------------------------------------------- theme colors
namespace col {
    const COLORREF bg        = RGB(28, 28, 32);
    const COLORREF panel     = RGB(36, 37, 43);
    const COLORREF transport = RGB(44, 46, 54);
    const COLORREF card      = RGB(52, 54, 63);
    const COLORREF cardSel   = RGB(60, 74, 104);
    const COLORREF cardEdge  = RGB(70, 72, 84);
    const COLORREF waveBg    = RGB(40, 42, 50);
    const COLORREF wave      = RGB(96, 170, 240);
    const COLORREF waveSel   = RGB(255, 196, 92);
    const COLORREF selRect   = RGB(66, 78, 96);
    const COLORREF accent    = RGB(78, 202, 122);
    const COLORREF accentDk  = RGB(52, 150, 92);
    const COLORREF stop      = RGB(224, 96, 96);
    const COLORREF text      = RGB(232, 232, 238);
    const COLORREF dim       = RGB(158, 160, 172);
    const COLORREF btn       = RGB(62, 64, 74);
    const COLORREF btnHot    = RGB(78, 82, 96);
    const COLORREF playhead  = RGB(255, 240, 120);
    const COLORREF ruler     = RGB(30, 31, 37);
    const COLORREF clipBlk   = RGB(70, 108, 150);
    const COLORREF clipBlkSel= RGB(96, 140, 196);
}

// ----------------------------------------------------------------- transport ids
enum TB { TB_ADD, TB_TRACK, TB_PLAYALL, TB_STOP, TB_UNDO, TB_REDO, TB_COUNT };

// ----------------------------------------------------------------- context menu ids
enum {
    IDM_PLAY = 100, IDM_PLAYSEL, IDM_CLEARSEL, IDM_SAVESEL, IDM_CROP,
    IDM_RENAME, IDM_DELETE,
    IDM_NORM_MATCH, IDM_NORM_ALL, IDM_DENOISE,
    IDM_ADDTL_BASE = 200,   // + track index
    IDM_TL_REMOVE = 300, IDM_TL_REMOVE_TRACK,
    IDM_REDO_BASE = 400
};

// Menu bar command ids
enum {
    IDC_ADDFILES = 1000, IDC_OPENPROJ, IDC_SAVEPROJ, IDC_SAVEPROJAS,
    IDC_EXPORTMIX, IDC_EXIT,
    IDC_UNDO, IDC_REDO, IDC_ADDTRACK, IDC_CONTROLS
};

struct CardLayout { int clipId; RECT card, top, play, del, wave, vol; };
struct PlacedLayout { int trackId, index, clipId; RECT rc; };
struct TrackLayout { int trackId; RECT header, lane; RECT nameRc, delRc, volRc; };

enum class Mode { None, WaveSelect, CardDrag, ClipMove, TimelineSeek, ClipVolume, TrackVolume };

struct App {
    HWND hwnd = nullptr;
    Document doc;
    PlaybackEngine engine;
    int rate = 48000;
    float sc = 1.0f;               // dpi scale
    HFONT fNorm=0, fSmall=0, fBold=0, fBig=0;

    RECT rcTransport{}, rcLibrary{}, rcTimeline{};
    RECT tbRects[TB_COUNT]{};

    std::wstring projectPath;      // current .acep path (empty = unsaved)

    // scroll / zoom
    int libScroll = 0, libContentH = 0;
    double pxPerSec = 90.0;
    int tlScrollX = 0, tlScrollY = 0, tlContentH = 0;
    int trackHeaderW = 128, rulerH = 22;

    // layout caches (rebuilt each layout())
    std::vector<CardLayout> cards;
    std::vector<PlacedLayout> placed;
    std::vector<TrackLayout> trackLays;

    // playback / preview state
    int previewClipId = -1;
    bool previewIsSel = false;
    int64_t previewCursor = 0;     // frame within clip (display + start point)
    bool timelinePlaying = false;
    int64_t playheadFrame = 0;

    // selection (belongs to selClipId)
    int selClipId = -1;
    int64_t selStart = 0, selEnd = 0;

    // interaction
    Mode mode = Mode::None;
    POINT downPt{};
    bool dragged = false;
    int dragClipId = -1;           // CardDrag / ClipMove source clip
    int volTrackId = -1;           // TrackVolume drag target
    int moveTrackId = -1, moveIndex = -1;
    int64_t moveGrabOffset = 0;    // frames from clip start to grab point
    int hotTB = -1;

    // ------------------------------------------------------------- helpers
    int S(int v) const { return (int)(v * sc + 0.5f); }
    bool hasSel() const { return selClipId >= 0 && selEnd > selStart; }

    void makeFonts() {
        if (fNorm) { DeleteObject(fNorm); DeleteObject(fSmall); DeleteObject(fBold); DeleteObject(fBig); }
        auto mk = [&](int h, int w) {
            return CreateFontW(-S(h), 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET,
                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
                L"Segoe UI");
        };
        fNorm = mk(14, FW_NORMAL); fSmall = mk(12, FW_NORMAL);
        fBold = mk(14, FW_SEMIBOLD); fBig = mk(17, FW_SEMIBOLD);
    }

    // frames <-> timeline pixel x (client coords)
    double pxPerFrame() const { return pxPerSec / rate; }
    int frameToX(int64_t f) const {
        return rcTimeline.left + trackHeaderW - tlScrollX + (int)(f * pxPerFrame());
    }
    int64_t xToFrame(int x) const {
        double f = (x - (rcTimeline.left + trackHeaderW) + tlScrollX) / pxPerFrame();
        return f < 0 ? 0 : (int64_t)f;
    }

    // --------------------------------------------------------- layout
    void computeLayout() {
        RECT rc; GetClientRect(hwnd, &rc);
        int tH = S(62);
        rcTransport = { 0, 0, rc.right, tH };
        int split = tH + (rc.bottom - tH) * 52 / 100;
        rcLibrary = { 0, tH, rc.right, split };
        rcTimeline = { 0, split, rc.right, rc.bottom };
        trackHeaderW = S(128);
        rulerH = S(22);
        layoutTransport();
        layoutCards();
        layoutTracks();
    }

    void layoutTransport() {
        int y = S(12), h = S(38), x = S(12);
        auto put = [&](TB id, int w) { tbRects[id] = { x, y, x + w, y + h }; x += w + S(8); };
        put(TB_ADD, S(96));
        put(TB_TRACK, S(96));
        x += S(16);
        put(TB_PLAYALL, S(140));
        put(TB_STOP, S(70));
        // undo/redo on the right
        int rx = rcTransport.right - S(12);
        tbRects[TB_REDO] = { rx - S(70), y, rx, y + h }; rx -= S(78);
        tbRects[TB_UNDO] = { rx - S(70), y, rx, y + h };
    }

    void layoutCards() {
        cards.clear();
        const auto& lib = doc.project().library;
        int pad = S(12);
        int cardW = S(300), cardH = S(120);
        int x = rcLibrary.left + pad;
        int y = rcLibrary.top + pad - libScroll;
        int rowH = cardH + pad;
        int maxRight = rcLibrary.right - pad;
        int startY = rcLibrary.top + pad - libScroll;
        int rows = 1;
        for (const auto& c : lib) {
            if (x + cardW > maxRight && x > rcLibrary.left + pad) {
                x = rcLibrary.left + pad; y += rowH; rows++;
            }
            CardLayout cl; cl.clipId = c.id;
            cl.card = { x, y, x + cardW, y + cardH };
            cl.top = { x, y, x + cardW, y + S(24) };
            cl.play = { x + S(6), y + S(4), x + S(6) + S(16), y + S(4) + S(16) };
            cl.del = { x + cardW - S(20), y + S(4), x + cardW - S(4), y + S(20) };
            cl.wave = { x + S(8), y + S(28), x + cardW - S(8), y + cardH - S(30) };
            cl.vol  = { x + S(40), y + cardH - S(24), x + cardW - S(44), y + cardH - S(8) };
            cards.push_back(cl);
            x += cardW + pad;
        }
        // content height for scroll clamp
        int used = (int)lib.size();
        int perRow = std::max(1, (int)(rcLibrary.right - 2 * pad) / (cardW + pad));
        int numRows = (used + perRow - 1) / perRow;
        libContentH = pad + numRows * rowH;
    }

    void layoutTracks() {
        trackLays.clear(); placed.clear();
        const auto& tracks = doc.project().tracks;
        int laneH = S(72);
        int y = rcTimeline.top + rulerH - tlScrollY;
        int laneLeft = rcTimeline.left + trackHeaderW;
        for (int ti = 0; ti < (int)tracks.size(); ++ti) {
            const Track& t = tracks[ti];
            TrackLayout tl; tl.trackId = t.id;
            tl.header = { rcTimeline.left, y, laneLeft, y + laneH };
            tl.lane = { laneLeft, y, rcTimeline.right, y + laneH };
            tl.nameRc = { tl.header.left + S(8), y + S(6), tl.header.right - S(8), y + S(24) };
            tl.volRc = { tl.header.left + S(34), y + laneH - S(24), tl.header.right - S(10), y + laneH - S(8) };
            tl.delRc = { tl.header.right - S(28), y + S(6), tl.header.right - S(8), y + S(24) };
            trackLays.push_back(tl);
            for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
                const PlacedClip& pc = t.clips[ci];
                int x0 = frameToX(pc.startFrame);
                int x1 = frameToX(pc.endFrame());
                PlacedLayout pl; pl.trackId = t.id; pl.index = ci; pl.clipId = pc.clipId;
                pl.rc = { x0, y + S(4), x1, y + laneH - S(4) };
                placed.push_back(pl);
            }
            y += laneH + S(6);
        }
        tlContentH = (y + tlScrollY) - (rcTimeline.top + rulerH) + S(6);
    }

    // --------------------------------------------------------- painting
    void paint(HDC hdc, const RECT& client) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        HGDIOBJ oldb = SelectObject(mem, bmp);
        SetBkMode(mem, TRANSPARENT);

        fill(mem, client, col::bg);
        paintLibrary(mem);
        paintTimeline(mem);
        paintTransport(mem);

        BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldb); DeleteObject(bmp); DeleteDC(mem);
    }

    static void fill(HDC h, const RECT& r, COLORREF c) {
        HBRUSH b = CreateSolidBrush(c); FillRect(h, &r, b); DeleteObject(b);
    }
    static void roundFill(HDC h, RECT r, COLORREF c, COLORREF edge, int rad) {
        HBRUSH b = CreateSolidBrush(c); HPEN p = CreatePen(PS_SOLID, 1, edge);
        HGDIOBJ ob = SelectObject(h, b), op = SelectObject(h, p);
        RoundRect(h, r.left, r.top, r.right, r.bottom, rad, rad);
        SelectObject(h, ob); SelectObject(h, op); DeleteObject(b); DeleteObject(p);
    }
    void textOut(HDC h, const RECT& r, const std::wstring& s, COLORREF c, HFONT f, UINT fmt) {
        SetTextColor(h, c); HGDIOBJ of = SelectObject(h, f);
        RECT rr = r; DrawTextW(h, s.c_str(), -1, &rr, fmt | DT_NOPREFIX);
        SelectObject(h, of);
    }

    static std::wstring fmtTime(double sec) {
        if (sec < 0) sec = 0;
        int m = (int)(sec / 60); double s = sec - m * 60;
        wchar_t b[32]; swprintf(b, 32, L"%d:%05.2f", m, s); return b;
    }

    void button(HDC h, const RECT& r, const std::wstring& label, COLORREF bg, COLORREF fg,
                bool hot, HFONT f) {
        roundFill(h, r, hot ? col::btnHot : bg, col::cardEdge, S(6));
        textOut(h, r, label, fg, f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Volume slider: track range is linear gain 0..2 (1.0 at the midpoint).
    static float gainToFrac(float g) { float f = g * 0.5f; return f < 0 ? 0 : (f > 1 ? 1 : f); }
    float sliderGainAt(const RECT& r, int x) const {
        float f = (float)(x - r.left) / std::max(1, (int)(r.right - r.left));
        f = std::max(0.0f, std::min(1.0f, f));
        return f * 2.0f;
    }
    // Slightly expanded hit rect so the thin slider is easy to grab.
    RECT sliderHit(const RECT& r) const { return { r.left - S(6), r.top - S(6), r.right + S(6), r.bottom + S(6) }; }

    void drawSlider(HDC h, const RECT& r, float gain) {
        int cy = (r.top + r.bottom) / 2;
        RECT trk = { r.left, cy - S(2), r.right, cy + S(2) };
        roundFill(h, trk, col::btn, col::cardEdge, S(2));
        float frac = gainToFrac(gain);
        int kx = r.left + (int)((r.right - r.left) * frac);
        RECT fillr = { r.left, cy - S(2), kx, cy + S(2) };
        HBRUSH b = CreateSolidBrush(col::accent); FillRect(h, &fillr, b); DeleteObject(b);
        RECT knob = { kx - S(4), cy - S(6), kx + S(4), cy + S(6) };
        roundFill(h, knob, col::text, col::accentDk, S(3));
    }

    static std::wstring gainLabel(float g) {
        wchar_t b[16]; swprintf(b, 16, L"%d%%", (int)(g * 100.0f + 0.5f)); return b;
    }

    void paintTransport(HDC h) {
        fill(h, rcTransport, col::transport);
        button(h, tbRects[TB_ADD], L"+ Add Files", col::btn, col::text, hotTB == TB_ADD, fNorm);
        button(h, tbRects[TB_TRACK], L"+ Add Track", col::btn, col::text, hotTB == TB_TRACK, fNorm);

        bool tlPlaying = timelinePlaying && engine.isPlaying();
        std::wstring pa = tlPlaying ? L"\u275A\u275A  Pause" : L"\u25B6  Play All";
        button(h, tbRects[TB_PLAYALL], pa, col::accentDk, col::text, hotTB == TB_PLAYALL, fBig);
        button(h, tbRects[TB_STOP], L"\u25A0 Stop", col::btn, col::text, hotTB == TB_STOP, fNorm);

        // time readout
        double posSec, totSec;
        if (timelinePlaying) { posSec = (double)playheadFrame / rate; totSec = (double)doc.project().timelineLengthFrames() / rate; }
        else if (previewClipId >= 0) {
            posSec = (double)previewCursor / rate;
            const Clip* c = doc.project().findClip(previewClipId);
            totSec = c ? c->durationSec() : 0;
        } else { posSec = (double)playheadFrame / rate; totSec = (double)doc.project().timelineLengthFrames() / rate; }
        RECT tr = { tbRects[TB_STOP].right + S(16), rcTransport.top, tbRects[TB_UNDO].left - S(12), rcTransport.bottom };
        textOut(h, tr, fmtTime(posSec) + L"  /  " + fmtTime(totSec), col::text, fBig, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        COLORREF uf = doc.canUndo() ? col::text : col::dim;
        COLORREF rf = doc.canRedo() ? col::text : col::dim;
        button(h, tbRects[TB_UNDO], L"\u21B6 Undo", col::btn, uf, hotTB == TB_UNDO, fNorm);
        button(h, tbRects[TB_REDO], L"\u21B7 Redo", col::btn, rf, hotTB == TB_REDO, fNorm);
    }

    void paintLibrary(HDC h) {
        fill(h, rcLibrary, col::panel);
        RECT hdr = rcLibrary; hdr.bottom = hdr.top; // no header bar; draw hint if empty
        SaveDC(h); IntersectClipRect(h, rcLibrary.left, rcLibrary.top, rcLibrary.right, rcLibrary.bottom);

        if (doc.project().library.empty()) {
            RECT r = rcLibrary;
            textOut(h, r, L"Click \u201C+ Add Files\u201D to load audio clips.\n"
                          L"Drag a clip's title bar onto a track below to place it.",
                    col::dim, fNorm, DT_CENTER | DT_VCENTER);
        }

        for (const auto& cl : cards) {
            const Clip* c = doc.project().findClip(cl.clipId);
            if (!c) continue;
            bool selHere = (selClipId == cl.clipId);
            roundFill(h, cl.card, selHere ? col::cardSel : col::card, col::cardEdge, S(8));

            // play/pause button
            bool playingThis = (previewClipId == cl.clipId) && engine.isPlaying();
            paintPlayIcon(h, cl.play, playingThis);

            // name + duration
            RECT nameRc = { cl.play.right + S(6), cl.top.top + S(2), cl.del.left - S(6), cl.top.bottom };
            textOut(h, nameRc, c->name, col::text, fBold, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            // delete x
            textOut(h, cl.del, L"\u2715", col::dim, fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // waveform
            fill(h, cl.wave, col::waveBg);
            if (c->peaks) {
                int64_t nf = c->frames();
                wf::draw(h, cl.wave, *c->peaks, 0, nf, col::wave);
                // selection overlay
                if (hasSel() && selClipId == cl.clipId) {
                    int wl = cl.wave.left, ww = cl.wave.right - cl.wave.left;
                    int sx0 = wl + (int)((double)selStart / nf * ww);
                    int sx1 = wl + (int)((double)selEnd / nf * ww);
                    RECT sr = { sx0, cl.wave.top, sx1, cl.wave.bottom };
                    // highlight rect + waveform redrawn in accent within it
                    fill(h, sr, col::selRect);
                    SaveDC(h); IntersectClipRect(h, sr.left, sr.top, sr.right, sr.bottom);
                    wf::draw(h, cl.wave, *c->peaks, 0, nf, col::waveSel);
                    RestoreDC(h, -1);
                    HPEN pen = CreatePen(PS_SOLID, 1, col::waveSel); HGDIOBJ op = SelectObject(h, pen);
                    MoveToEx(h, sx0, cl.wave.top, nullptr); LineTo(h, sx0, cl.wave.bottom);
                    MoveToEx(h, sx1, cl.wave.top, nullptr); LineTo(h, sx1, cl.wave.bottom);
                    SelectObject(h, op); DeleteObject(pen);
                }
                // preview cursor line
                if (previewClipId == cl.clipId) {
                    int wl = cl.wave.left, ww = cl.wave.right - cl.wave.left;
                    int cx = wl + (int)((double)previewCursor / std::max<int64_t>(1, nf) * ww);
                    HPEN pen = CreatePen(PS_SOLID, S(1), col::playhead); HGDIOBJ op = SelectObject(h, pen);
                    MoveToEx(h, cx, cl.wave.top, nullptr); LineTo(h, cx, cl.wave.bottom);
                    SelectObject(h, op); DeleteObject(pen);
                }
            }
            // duration inside the wave, bottom-right
            RECT dr = { cl.wave.left, cl.wave.bottom - S(15), cl.wave.right - S(3), cl.wave.bottom - S(2) };
            textOut(h, dr, fmtTime(c->durationSec()), col::dim, fSmall, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);

            // volume slider row
            RECT spk = { cl.card.left + S(8), cl.vol.top - S(2), cl.vol.left - S(2), cl.vol.bottom + S(2) };
            textOut(h, spk, L"\U0001F509", col::dim, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            drawSlider(h, cl.vol, c->gain);
            RECT pct = { cl.vol.right + S(4), cl.vol.top - S(2), cl.card.right - S(6), cl.vol.bottom + S(2) };
            textOut(h, pct, gainLabel(c->gain), col::dim, fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        RestoreDC(h, -1);
    }

    void paintPlayIcon(HDC h, const RECT& r, bool playing) {
        roundFill(h, r, col::accent, col::accentDk, S(4));
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = (r.right - r.left) / 4;
        HBRUSH b = CreateSolidBrush(col::bg); HGDIOBJ ob = SelectObject(h, b);
        HPEN pen = CreatePen(PS_SOLID, 1, col::bg); HGDIOBJ op = SelectObject(h, pen);
        if (playing) {
            RECT a = { cx - s, cy - s, cx - s / 2, cy + s };
            RECT bb = { cx + s / 2, cy - s, cx + s, cy + s };
            FillRect(h, &a, b); FillRect(h, &bb, b);
        } else {
            POINT tri[3] = { {cx - s + 1, cy - s}, {cx - s + 1, cy + s}, {cx + s, cy} };
            Polygon(h, tri, 3);
        }
        SelectObject(h, ob); SelectObject(h, op); DeleteObject(b); DeleteObject(pen);
    }

    void paintTimeline(HDC h) {
        fill(h, rcTimeline, col::bg);
        // ruler
        RECT rr = { rcTimeline.left + trackHeaderW, rcTimeline.top, rcTimeline.right, rcTimeline.top + rulerH };
        fill(h, rr, col::ruler);
        RECT rlh = { rcTimeline.left, rcTimeline.top, rcTimeline.left + trackHeaderW, rcTimeline.top + rulerH };
        fill(h, rlh, col::transport);
        textOut(h, rlh, L"Tracks", col::dim, fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        paintRulerTicks(h, rr);

        SaveDC(h); IntersectClipRect(h, rcTimeline.left, rcTimeline.top + rulerH, rcTimeline.right, rcTimeline.bottom);
        for (const auto& tl : trackLays) {
            const Track* t = nullptr;
            for (auto& tt : doc.project().tracks) if (tt.id == tl.trackId) t = &tt;
            // header
            fill(h, tl.header, col::transport);
            RECT edge = { tl.header.right - 1, tl.header.top, tl.header.right, tl.header.bottom };
            fill(h, edge, col::cardEdge);
            textOut(h, tl.nameRc, t ? t->name : L"", col::text, fBold, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            // track volume slider
            RECT tspk = { tl.header.left + S(8), tl.volRc.top - S(3), tl.volRc.left - S(2), tl.volRc.bottom + S(3) };
            textOut(h, tspk, L"\U0001F509", col::dim, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            drawSlider(h, tl.volRc, t ? t->gain : 1.0f);
            if (doc.project().tracks.size() > 1)
                textOut(h, tl.delRc, L"\u2715", col::dim, fSmall, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            // lane bg
            fill(h, tl.lane, col::panel);
            RECT laneln = { tl.lane.left, tl.lane.bottom - 1, tl.lane.right, tl.lane.bottom };
            fill(h, laneln, col::bg);
        }
        // placed clips
        for (const auto& pl : placed) {
            const Clip* c = doc.project().findClip(pl.clipId);
            bool moving = (mode == Mode::ClipMove && moveTrackId == pl.trackId && moveIndex == pl.index);
            roundFill(h, pl.rc, moving ? col::clipBlkSel : col::clipBlk, col::cardEdge, S(6));
            RECT wv = { pl.rc.left + S(3), pl.rc.top + S(18), pl.rc.right - S(3), pl.rc.bottom - S(4) };
            if (c && c->peaks && wv.right > wv.left) {
                SaveDC(h); IntersectClipRect(h, wv.left, wv.top, wv.right, wv.bottom);
                wf::draw(h, wv, *c->peaks, 0, c->frames(), RGB(180, 210, 245));
                RestoreDC(h, -1);
            }
            RECT nm = { pl.rc.left + S(6), pl.rc.top + S(2), pl.rc.right - S(4), pl.rc.top + S(18) };
            textOut(h, nm, c ? c->name : L"", col::text, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        // drag ghost when dropping a library clip
        if (mode == Mode::CardDrag && dragged) {
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            int tid; if (trackAtPoint(p, tid)) {
                const Clip* c = doc.project().findClip(dragClipId);
                if (c) {
                    int64_t start = snapFrame(tid, xToFrame(p.x - (int)(moveGrabOffset * pxPerFrame())), c->frames(), -1);
                    for (auto& tl : trackLays) if (tl.trackId == tid) {
                        int x0 = frameToX(start), x1 = frameToX(start + c->frames());
                        RECT g = { x0, tl.lane.top + S(4), x1, tl.lane.bottom - S(4) };
                        const Track* tk = nullptr; for (auto& tt : doc.project().tracks) if (tt.id == tid) tk = &tt;
                        bool ok = tk && !tk->overlaps(start, c->frames());
                        roundFill(h, g, ok ? col::accentDk : col::stop, col::text, S(6));
                    }
                }
            }
        }
        // playhead
        if (timelinePlaying || (!engine.isPlaying() && previewClipId < 0)) {
            int px = frameToX(playheadFrame);
            if (px >= rcTimeline.left + trackHeaderW && px <= rcTimeline.right) {
                HPEN pen = CreatePen(PS_SOLID, S(1), col::playhead); HGDIOBJ op = SelectObject(h, pen);
                MoveToEx(h, px, rcTimeline.top + rulerH, nullptr); LineTo(h, px, rcTimeline.bottom);
                SelectObject(h, op); DeleteObject(pen);
            }
        }
        RestoreDC(h, -1);
    }

    void paintRulerTicks(HDC h, const RECT& rr) {
        double secPerTick = 1.0;
        while (secPerTick * pxPerSec < S(60)) secPerTick *= 2;
        int laneLeft = rcTimeline.left + trackHeaderW;
        double startSec = tlScrollX / pxPerSec;
        double t = std::floor(startSec / secPerTick) * secPerTick;
        HPEN pen = CreatePen(PS_SOLID, 1, col::cardEdge); HGDIOBJ op = SelectObject(h, pen);
        for (; ; t += secPerTick) {
            int x = laneLeft - tlScrollX + (int)(t * pxPerSec);
            if (x > rr.right) break;
            if (x < laneLeft) continue;
            MoveToEx(h, x, rr.top, nullptr); LineTo(h, x, rr.bottom);
            RECT lab = { x + S(3), rr.top, x + S(60), rr.bottom };
            textOut(h, lab, fmtTime(t), col::dim, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(h, op); DeleteObject(pen);
    }

    // --------------------------------------------------------- hit testing
    const CardLayout* cardAt(POINT p) {
        for (auto& c : cards) if (PtInRect(&c.card, p)) return &c;
        return nullptr;
    }
    const PlacedLayout* placedAt(POINT p) {
        for (auto& pl : placed) if (PtInRect(&pl.rc, p)) return &pl;
        return nullptr;
    }
    bool trackAtPoint(POINT p, int& trackId) {
        for (auto& tl : trackLays)
            if (p.x >= tl.lane.left && p.x < tl.lane.right && p.y >= tl.lane.top && p.y < tl.lane.bottom) {
                trackId = tl.trackId; return true;
            }
        return false;
    }
    int tbAt(POINT p) {
        for (int i = 0; i < TB_COUNT; ++i) if (PtInRect(&tbRects[i], p)) return i;
        return -1;
    }

    int64_t snapFrame(int trackId, int64_t start, int64_t len, int ignoreIndex) {
        const Track* t = nullptr;
        for (auto& tt : doc.project().tracks) if (tt.id == trackId) t = &tt;
        if (!t) return std::max<int64_t>(0, start);
        int64_t best = start; int64_t bestD = (int64_t)(S(10) / pxPerFrame()) + 1;
        auto trySnap = [&](int64_t candidate) {
            int64_t d = std::llabs(candidate - start);
            if (d < bestD) { bestD = d; best = candidate; }
        };
        trySnap(0);
        for (int i = 0; i < (int)t->clips.size(); ++i) {
            if (i == ignoreIndex) continue;
            trySnap(t->clips[i].endFrame());           // butt up after a clip
            trySnap(t->clips[i].startFrame - len);     // butt up before a clip
        }
        if (best < 0) best = 0;
        return best;
    }

    // --------------------------------------------------------- actions
    void addFiles() {
        auto paths = dlg::openAudioFiles(hwnd);
        int lastId = -1;
        for (auto& p : paths) {
            std::wstring err;
            auto buf = mfio::decodeFile(p, rate, &err);
            if (!buf) { MessageBoxW(hwnd, err.c_str(), L"Could not load", MB_ICONWARNING); continue; }
            std::wstring name = PathFindFileNameW(p.c_str());
            size_t dot = name.find_last_of(L'.'); if (dot != std::wstring::npos) name = name.substr(0, dot);
            lastId = doc.addClip(name, buf, p, L"Add clip '" + name + L"'");
        }
        clampScroll(); refresh();
    }

    void togglePlayClip(int clipId) {
        if (previewClipId == clipId && !previewIsSel) {
            if (engine.isPlaying()) engine.pause();
            else if (engine.isPaused()) engine.resume();
            else startClipPreview(clipId, previewCursor);
        } else {
            startClipPreview(clipId, (selClipId == clipId ? 0 : 0));
        }
        refresh();
    }
    void startClipPreview(int clipId, int64_t startFrame) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer) return;
        if (startFrame < 0 || startFrame >= c->frames()) startFrame = 0;  // restart if at end
        timelinePlaying = false;
        previewClipId = clipId; previewIsSel = false; previewCursor = startFrame;
        engine.play(std::make_shared<BufferSource>(c->buffer, 0, c->frames(), c->gain), startFrame);
    }
    void playSelection() {
        if (!hasSel()) return;
        const Clip* c = doc.project().findClip(selClipId);
        if (!c || !c->buffer) return;
        timelinePlaying = false;
        previewClipId = selClipId; previewIsSel = true; previewCursor = selStart;
        engine.play(std::make_shared<BufferSource>(c->buffer, selStart, selEnd, c->gain), 0);
        refresh();
    }
    void seekClip(int clipId, int64_t frame) {
        previewClipId = clipId; previewIsSel = false; previewCursor = frame; timelinePlaying = false;
        if (engine.hasSource() && engine.isPlaying()) {
            const Clip* c = doc.project().findClip(clipId);
            if (c) engine.play(std::make_shared<BufferSource>(c->buffer, 0, c->frames(), c->gain), frame);
        }
        refresh();
    }

    AudioBufferPtr sliceBuffer(const AudioBuffer& src, int64_t f0, int64_t f1) {
        auto out = std::make_shared<AudioBuffer>();
        out->sampleRate = src.sampleRate; out->channels = src.channels;
        int ch = src.channels;
        f0 = std::max<int64_t>(0, f0); f1 = std::min<int64_t>(src.frames(), f1);
        if (f1 > f0)
            out->samples.assign(src.samples.begin() + f0 * ch, src.samples.begin() + f1 * ch);
        return out;
    }

    void saveSelectionAsClip() {
        if (!hasSel()) return;
        const Clip* c = doc.project().findClip(selClipId);
        if (!c || !c->buffer) return;
        std::wstring name = c->name + L" (clip)";
        if (!dlg::promptText(hwnd, L"Save selection as new clip", L"Clip name:", name)) return;
        auto slice = sliceBuffer(*c->buffer, selStart, selEnd);
        doc.addClip(name, slice, L"", L"Save selection as '" + name + L"'");
        clampScroll(); refresh();
    }

    void cropSelection() {
        if (!hasSel()) return;
        Clip* c = doc.project().findClip(selClipId);
        if (!c || !c->buffer) return;
        int id = c->id; std::wstring nm = c->name; std::wstring srcPath = c->sourcePath;
        auto slice = sliceBuffer(*c->buffer, selStart, selEnd);

        int choice = MessageBoxW(hwnd,
            L"Crop this clip to the current selection.\n\n"
            L"YES  - overwrite the original file on disk\n"
            L"NO   - save the cropped audio as a new file\n"
            L"CANCEL - just trim the clip in the editor",
            L"Crop clip", MB_YESNOCANCEL | MB_ICONQUESTION);

        // Always trim the in-editor clip (undoable) unless the user cancelled outright?
        // Cancel here means "just trim in editor" per the prompt.
        doc.replaceClipBuffer(id, slice, L"Crop '" + nm + L"'");
        // reset selection to whole new clip
        selStart = 0; selEnd = 0; selClipId = -1;

        if (choice == IDYES) {
            if (srcPath.empty()) {
                MessageBoxW(hwnd, L"This clip has no source file; use Save As instead.", L"Crop", MB_ICONINFORMATION);
            } else {
                mfio::ExportOptions o; o.sampleRate = rate; o.channels = slice->channels;
                o.format = formatFromExt(srcPath); o.bitrateKbps = 192;
                std::wstring err;
                if (!mfio::encodeFile(srcPath, *slice, o, &err))
                    MessageBoxW(hwnd, err.c_str(), L"Could not overwrite", MB_ICONWARNING);
            }
        } else if (choice == IDNO) {
            mfio::ExportOptions o; o.sampleRate = rate; o.channels = slice->channels; o.format = mfio::ExportFormat::WAV;
            std::wstring outPath;
            if (dlg::exportOptions(hwnd, o, outPath, nm)) {
                std::wstring err;
                if (!mfio::encodeFile(outPath, *slice, o, &err))
                    MessageBoxW(hwnd, err.c_str(), L"Could not save", MB_ICONWARNING);
            }
        }
        refresh();
    }

    static mfio::ExportFormat formatFromExt(const std::wstring& path) {
        std::wstring e = PathFindExtensionW(path.c_str());
        for (auto& c : e) c = towlower(c);
        if (e == L".mp3") return mfio::ExportFormat::MP3;
        if (e == L".m4a" || e == L".aac" || e == L".mp4") return mfio::ExportFormat::AAC;
        if (e == L".wma") return mfio::ExportFormat::WMA;
        return mfio::ExportFormat::WAV;
    }

    void renameClip(int id) {
        const Clip* c = doc.project().findClip(id);
        if (!c) return;
        std::wstring name = c->name;
        if (dlg::promptText(hwnd, L"Rename clip", L"Clip name:", name)) { doc.renameClip(id, name); refresh(); }
    }

    void normalizeClip(int clipId, bool all) {
        int n = doc.normalizeClips(all ? -1 : clipId, all);
        refresh();
        if (n == 0)
            MessageBoxW(hwnd, L"Nothing to normalize \u2014 clips are silent or already matched.",
                        L"Normalize", MB_ICONINFORMATION);
    }

    void voiceCleanClip(int clipId) {
        Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer) return;
        std::wstring name = c->name;
        dsp::NROptions opts;
        opts.algorithm = dsp::NRAlgorithm::SpectralSubtraction;
        opts.strength = dsp::NRStrength::Medium;
        if (!dlg::voiceCleaner(hwnd, opts)) return;
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        auto cleaned = dsp::denoise(*c->buffer, opts);
        SetCursor(old);
        if (!cleaned) { MessageBoxW(hwnd, L"Voice cleaner failed.", L"Voice cleaner", MB_ICONWARNING); return; }
        if (previewClipId == clipId) stopAll();
        doc.replaceClipBuffer(clipId, cleaned, L"Voice cleaner on '" + name + L"'");
        refresh();
    }

    // --------------------------------------------------------- project I/O
    void setTitle() {
        std::wstring t = L"Audio Clip Editor";
        if (!projectPath.empty()) t += std::wstring(L" \u2014 ") + PathFindFileNameW(projectPath.c_str());
        SetWindowTextW(hwnd, t.c_str());
    }
    void openProjectFile() {
        std::wstring path = dlg::openProject(hwnd);
        if (path.empty()) return;
        if (!doc.loadProject(path)) { MessageBoxW(hwnd, L"Could not open project.", L"Open", MB_ICONWARNING); return; }
        stopAll();
        projectPath = path;
        selClipId = -1; selStart = selEnd = 0; previewClipId = -1; timelinePlaying = false; playheadFrame = 0;
        libScroll = tlScrollX = tlScrollY = 0;
        setTitle(); clampScroll(); refresh();
    }
    bool saveProjectAs() {
        std::wstring suggested = projectPath.empty() ? L"Untitled" : PathFindFileNameW(projectPath.c_str());
        std::wstring path = dlg::saveProject(hwnd, suggested);
        if (path.empty()) return false;
        if (!doc.saveProject(path)) { MessageBoxW(hwnd, L"Could not save project.", L"Save", MB_ICONWARNING); return false; }
        projectPath = path; setTitle(); return true;
    }
    void saveProjectFile() {
        if (projectPath.empty()) { saveProjectAs(); return; }
        if (!doc.saveProject(projectPath))
            MessageBoxW(hwnd, L"Could not save project.", L"Save", MB_ICONWARNING);
    }
    void exportMix() {
        auto mix = doc.renderMix();
        if (!mix || mix->frames() == 0) {
            MessageBoxW(hwnd, L"Nothing to export \u2014 place some clips on the timeline first.",
                        L"Export mixdown", MB_ICONINFORMATION);
            return;
        }
        mfio::ExportOptions o; o.sampleRate = rate; o.channels = 2; o.format = mfio::ExportFormat::WAV; o.bitrateKbps = 192;
        std::wstring outPath;
        if (!dlg::exportOptions(hwnd, o, outPath, L"Mixdown")) return;
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        std::wstring err; bool ok = mfio::encodeFile(outPath, *mix, o, &err);
        SetCursor(old);
        if (!ok) MessageBoxW(hwnd, err.c_str(), L"Could not export", MB_ICONWARNING);
    }

    void showControls() {
        MessageBoxW(hwnd,
            L"Library clips:\n"
            L"  \u2022 Click the \u25B6 button to play/pause a clip\n"
            L"  \u2022 Click-drag across the waveform to select a section\n"
            L"  \u2022 Drag the title bar down onto a track to place it\n"
            L"  \u2022 Right-click for save-selection, crop, normalize, voice cleaner\u2026\n"
            L"  \u2022 Drag the volume slider to change a clip's level\n\n"
            L"Timeline:\n"
            L"  \u2022 Drag placed clips to move them (snaps to neighbours)\n"
            L"  \u2022 Drag a track's volume slider to change the track level\n"
            L"  \u2022 Click a lane or the ruler to move the playhead\n"
            L"  \u2022 Ctrl+wheel zooms, Shift+wheel scrolls vertically\n\n"
            L"Keys:  Space = play/pause   Ctrl+Z = undo   Ctrl+Shift+Z = redo",
            L"Controls", MB_ICONINFORMATION);
    }

    void playAll() {
        if (timelinePlaying && engine.isPlaying()) { engine.pause(); refresh(); return; }
        if (timelinePlaying && engine.isPaused()) { engine.resume(); refresh(); return; }
        // build snapshot
        std::vector<TimelineSegment> segs;
        for (auto& t : doc.project().tracks) {
            if (t.muted) continue;
            for (auto& pc : t.clips) {
                const Clip* c = doc.project().findClip(pc.clipId);
                if (!c || !c->buffer) continue;
                TimelineSegment s; s.buf = c->buffer; s.timelineStart = pc.startFrame; s.length = pc.lengthFrames;
                s.gain = c->gain * t.gain;
                segs.push_back(s);
            }
        }
        int64_t total = doc.project().timelineLengthFrames();
        if (total <= 0) return;
        previewClipId = -1; previewIsSel = false; timelinePlaying = true;
        engine.play(std::make_shared<TimelineSource>(std::move(segs), total), playheadFrame);
        refresh();
    }
    void stopAll() {
        engine.stop(); timelinePlaying = false; previewClipId = -1; refresh();
    }

    // --------------------------------------------------------- undo / redo
    void doUndo() { if (doc.canUndo()) { syncSelectionAfterEdit(); doc.undo(); afterHistory(); } }
    void doRedo() {
        if (!doc.canRedo()) return;
        int branch = doc.defaultRedoBranch();
        if (doc.redoBranchCount() > 1) {
            HMENU m = CreatePopupMenu();
            for (int i = 0; i < doc.redoBranchCount(); ++i)
                AppendMenuW(m, MF_STRING, IDM_REDO_BASE + i, doc.redoChildDesc(i).c_str());
            POINT p; GetCursorPos(&p);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(m);
            if (cmd >= IDM_REDO_BASE) branch = cmd - IDM_REDO_BASE; else return;
        }
        doc.redo(branch); afterHistory();
    }
    void syncSelectionAfterEdit() {}
    void afterHistory() {
        // invalidate playback that may reference stale buffers
        stopAll();
        selClipId = -1; selStart = selEnd = 0;
        clampScroll(); refresh();
    }

    // --------------------------------------------------------- scroll
    void clampScroll() {
        computeLayout();
        int visH = rcLibrary.bottom - rcLibrary.top;
        int maxS = std::max(0, libContentH - visH);
        libScroll = std::max(0, std::min(libScroll, maxS));
        int64_t total = doc.project().timelineLengthFrames();
        int maxX = std::max(0, (int)(total * pxPerFrame()) - (int)(rcTimeline.right - rcTimeline.left - trackHeaderW) + S(200));
        tlScrollX = std::max(0, std::min(tlScrollX, maxX));
        int tlVis = rcTimeline.bottom - rcTimeline.top - rulerH;
        int maxY = std::max(0, tlContentH - tlVis);
        tlScrollY = std::max(0, std::min(tlScrollY, maxY));
    }

    void refresh() { InvalidateRect(hwnd, nullptr, FALSE); }

    // --------------------------------------------------------- mouse
    void onLDown(POINT p, bool dbl) {
        SetFocus(hwnd);
        downPt = p; dragged = false;
        int tb = tbAt(p);
        if (PtInRect(&rcTransport, p)) { if (tb >= 0) handleTB(tb); return; }

        // library
        if (PtInRect(&rcLibrary, p)) {
            const CardLayout* cl = cardAt(p);
            if (!cl) { return; }
            if (PtInRect(&cl->play, p)) { togglePlayClip(cl->clipId); return; }
            if (PtInRect(&cl->del, p)) { deleteClipConfirm(cl->clipId); return; }
            { RECT vh = sliderHit(cl->vol);
              if (PtInRect(&vh, p)) {
                mode = Mode::ClipVolume; dragClipId = cl->clipId;
                if (Clip* c = doc.project().findClip(cl->clipId)) c->gain = sliderGainAt(cl->vol, p.x);
                SetCapture(hwnd); refresh(); return;
              } }
            if (PtInRect(&cl->wave, p)) {
                // begin selection / seek
                mode = Mode::WaveSelect; dragClipId = cl->clipId;
                const Clip* c = doc.project().findClip(cl->clipId);
                int64_t f = waveFrameAt(*cl, c ? c->frames() : 0, p.x);
                selClipId = cl->clipId; selStart = f; selEnd = f;
                SetCapture(hwnd); refresh(); return;
            }
            if (PtInRect(&cl->top, p)) {
                if (dbl) { renameClip(cl->clipId); return; }
                // start drag-to-timeline
                mode = Mode::CardDrag; dragClipId = cl->clipId; moveGrabOffset = 0;
                SetCapture(hwnd); return;
            }
            return;
        }

        // timeline
        if (PtInRect(&rcTimeline, p)) {
            // header hits
            for (auto& tl : trackLays) {
                if (PtInRect(&tl.delRc, p) && doc.project().tracks.size() > 1) {
                    doc.removeTrack(tl.trackId); afterHistory(); return;
                }
                { RECT vh = sliderHit(tl.volRc);
                  if (PtInRect(&vh, p) && p.x < tl.header.right) {
                    mode = Mode::TrackVolume; volTrackId = tl.trackId;
                    if (Track* t = doc.project().findTrack(tl.trackId)) t->gain = sliderGainAt(tl.volRc, p.x);
                    SetCapture(hwnd); refresh(); return;
                  } }
                if (PtInRect(&tl.nameRc, p) && dbl) {
                    const Track* t=nullptr; for(auto&tt:doc.project().tracks) if(tt.id==tl.trackId)t=&tt;
                    std::wstring nm = t?t->name:L""; if (dlg::promptText(hwnd,L"Rename track",L"Track name:",nm)){doc.renameTrack(tl.trackId,nm);refresh();} return;
                }
            }
            const PlacedLayout* pl = placedAt(p);
            if (pl) {
                mode = Mode::ClipMove; moveTrackId = pl->trackId; moveIndex = pl->index; dragClipId = pl->clipId;
                const Track* t=nullptr; for(auto&tt:doc.project().tracks) if(tt.id==pl->trackId)t=&tt;
                int64_t clipStart = t->clips[pl->index].startFrame;
                moveGrabOffset = xToFrame(p.x) - clipStart;
                SetCapture(hwnd); return;
            }
            // seek playhead by clicking a lane / ruler
            int tid;
            if (trackAtPoint(p, tid) || (p.y < rcTimeline.top + rulerH && p.x > rcTimeline.left + trackHeaderW)) {
                playheadFrame = xToFrame(p.x);
                if (timelinePlaying) engine.seek(playheadFrame);
                mode = Mode::TimelineSeek; SetCapture(hwnd); refresh(); return;
            }
        }
    }

    int64_t waveFrameAt(const CardLayout& cl, int64_t nf, int x) {
        double t = (double)(x - cl.wave.left) / std::max(1, (int)(cl.wave.right - cl.wave.left));
        t = std::max(0.0, std::min(1.0, t));
        return (int64_t)(t * nf);
    }

    void onMouseMove(POINT p) {
        int oldHot = hotTB;
        hotTB = PtInRect(&rcTransport, p) ? tbAt(p) : -1;
        if (hotTB != oldHot) refresh();

        if (mode == Mode::None) return;
        if (!dragged) {
            if (std::abs(p.x - downPt.x) > S(4) || std::abs(p.y - downPt.y) > S(4)) dragged = true;
        }
        if (mode == Mode::WaveSelect) {
            const CardLayout* cl = nullptr; for (auto& c : cards) if (c.clipId == dragClipId) cl = &c;
            if (cl) {
                const Clip* c = doc.project().findClip(dragClipId);
                int64_t f = waveFrameAt(*cl, c ? c->frames() : 0, p.x);
                selEnd = f; refresh();
            }
        } else if (mode == Mode::ClipVolume) {
            const CardLayout* cl = nullptr; for (auto& c : cards) if (c.clipId == dragClipId) cl = &c;
            if (cl) if (Clip* c = doc.project().findClip(dragClipId)) { c->gain = sliderGainAt(cl->vol, p.x); refresh(); }
        } else if (mode == Mode::TrackVolume) {
            const TrackLayout* tl = nullptr; for (auto& t : trackLays) if (t.trackId == volTrackId) tl = &t;
            if (tl) if (Track* t = doc.project().findTrack(volTrackId)) { t->gain = sliderGainAt(tl->volRc, p.x); refresh(); }
        } else if (mode == Mode::CardDrag || mode == Mode::ClipMove || mode == Mode::TimelineSeek) {
            if (mode == Mode::TimelineSeek) { playheadFrame = xToFrame(p.x); if (timelinePlaying) engine.seek(playheadFrame); }
            refresh();
        }
    }

    void onLUp(POINT p) {
        if (GetCapture() == hwnd) ReleaseCapture();
        Mode m = mode; mode = Mode::None;

        if (m == Mode::WaveSelect) {
            if (!dragged) {
                // plain click => seek, clear selection
                selClipId = -1; selStart = selEnd = 0;
                const CardLayout* cl = nullptr; for (auto& c : cards) if (c.clipId == dragClipId) cl = &c;
                if (cl) {
                    const Clip* c = doc.project().findClip(dragClipId);
                    seekClip(dragClipId, waveFrameAt(*cl, c ? c->frames() : 0, p.x));
                }
            } else {
                if (selEnd < selStart) std::swap(selStart, selEnd);
            }
            refresh();
        } else if (m == Mode::CardDrag) {
            if (dragged) {
                int tid;
                if (trackAtPoint(p, tid)) {
                    const Clip* c = doc.project().findClip(dragClipId);
                    if (c) {
                        int64_t start = snapFrame(tid, xToFrame(p.x - (int)(moveGrabOffset * pxPerFrame())), c->frames(), -1);
                        if (!doc.placeClip(tid, dragClipId, start, L"Add '" + c->name + L"' to timeline"))
                            MessageBoxW(hwnd, L"Clips can't overlap on a track.", L"Can't place", MB_ICONINFORMATION);
                        else afterPlaceRefresh();
                    }
                }
            }
        } else if (m == Mode::ClipMove) {
            if (dragged) {
                int tid = moveTrackId; trackAtPoint(p, tid);
                const Clip* c = doc.project().findClip(dragClipId);
                int64_t newStart = snapFrame(tid, xToFrame(p.x) - moveGrabOffset, c ? c->frames() : 0,
                                             tid == moveTrackId ? moveIndex : -1);
                doc.moveClip(moveTrackId, moveIndex, tid, newStart, L"Move clip");
                afterPlaceRefresh();
            }
        } else if (m == Mode::ClipVolume) {
            doc.commitEdit(L"Set clip volume"); refresh();
        } else if (m == Mode::TrackVolume) {
            doc.commitEdit(L"Set track volume"); refresh();
        }
        refresh();
    }
    void afterPlaceRefresh() { clampScroll(); refresh(); }

    void onRDown(POINT p) {
        if (PtInRect(&rcLibrary, p)) {
            const CardLayout* cl = cardAt(p);
            if (cl) { clipContextMenu(cl->clipId, p); return; }
        }
        if (PtInRect(&rcTimeline, p)) {
            const PlacedLayout* pl = placedAt(p);
            if (pl) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, IDM_TL_REMOVE, L"Remove from timeline");
                POINT sp = p; ClientToScreen(hwnd, &sp);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD, sp.x, sp.y, 0, hwnd, nullptr);
                DestroyMenu(m);
                if (cmd == IDM_TL_REMOVE) { doc.removePlaced(pl->trackId, pl->index, L"Remove clip from track"); afterHistory(); }
            }
        }
    }

    void clipContextMenu(int clipId, POINT p) {
        HMENU m = CreatePopupMenu();
        bool sel = hasSel() && selClipId == clipId;
        AppendMenuW(m, MF_STRING, IDM_PLAY, L"Play / Pause");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_PLAYSEL, L"Play selection");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_SAVESEL, L"Save selection as new clip");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_CROP, L"Crop to selection\u2026");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_CLEARSEL, L"Clear selection");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        // add-to-timeline submenu
        HMENU sub = CreatePopupMenu();
        auto& tracks = doc.project().tracks;
        for (int i = 0; i < (int)tracks.size(); ++i)
            AppendMenuW(sub, MF_STRING, IDM_ADDTL_BASE + i, tracks[i].name.c_str());
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Add to timeline");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_NORM_MATCH, L"Normalize to match other clips");
        AppendMenuW(m, MF_STRING, IDM_NORM_ALL, L"Normalize all clips (all tracks)");
        AppendMenuW(m, MF_STRING, IDM_DENOISE, L"Voice cleaner (reduce noise)\u2026");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_RENAME, L"Rename\u2026");
        AppendMenuW(m, MF_STRING, IDM_DELETE, L"Delete clip");

        POINT sp = p; ClientToScreen(hwnd, &sp);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD, sp.x, sp.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 0) return;
        if (cmd == IDM_PLAY) togglePlayClip(clipId);
        else if (cmd == IDM_PLAYSEL) { selClipId = clipId; playSelection(); }
        else if (cmd == IDM_SAVESEL) { selClipId = clipId; saveSelectionAsClip(); }
        else if (cmd == IDM_CROP) { selClipId = clipId; cropSelection(); }
        else if (cmd == IDM_CLEARSEL) { selClipId = -1; selStart = selEnd = 0; refresh(); }
        else if (cmd == IDM_NORM_MATCH) normalizeClip(clipId, false);
        else if (cmd == IDM_NORM_ALL) normalizeClip(clipId, true);
        else if (cmd == IDM_DENOISE) voiceCleanClip(clipId);
        else if (cmd == IDM_RENAME) renameClip(clipId);
        else if (cmd == IDM_DELETE) deleteClipConfirm(clipId);
        else if (cmd >= IDM_ADDTL_BASE && cmd < IDM_TL_REMOVE) {
            int idx = cmd - IDM_ADDTL_BASE;
            if (idx < (int)tracks.size()) {
                const Clip* c = doc.project().findClip(clipId);
                // place at end of the track (no overlap)
                int64_t start = 0;
                for (auto& pc : tracks[idx].clips) start = std::max(start, pc.endFrame());
                if (c) { doc.placeClip(tracks[idx].id, clipId, start, L"Add '" + c->name + L"' to timeline"); afterPlaceRefresh(); }
            }
        }
    }

    void deleteClipConfirm(int clipId) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c) return;
        if (MessageBoxW(hwnd, (L"Delete clip \"" + c->name + L"\"?\nThis also removes it from any tracks.").c_str(),
                        L"Delete clip", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            if (previewClipId == clipId) stopAll();
            doc.removeClip(clipId); afterHistory();
        }
    }

    void handleTB(int id) {
        switch (id) {
        case TB_ADD: addFiles(); break;
        case TB_TRACK: doc.addTrack(); afterPlaceRefresh(); break;
        case TB_PLAYALL: playAll(); break;
        case TB_STOP: stopAll(); break;
        case TB_UNDO: doUndo(); break;
        case TB_REDO: doRedo(); break;
        }
    }

    // --------------------------------------------------------- menu bar
    void buildMenu() {
        HMENU bar = CreateMenu();
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDC_ADDFILES, L"Add Files\u2026\tCtrl+O");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDC_OPENPROJ, L"Open Project\u2026");
        AppendMenuW(file, MF_STRING, IDC_SAVEPROJ, L"Save Project\tCtrl+S");
        AppendMenuW(file, MF_STRING, IDC_SAVEPROJAS, L"Save Project As\u2026");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDC_EXPORTMIX, L"Export Mixdown\u2026");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDC_EXIT, L"Exit");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"File");

        HMENU edit = CreatePopupMenu();
        AppendMenuW(edit, MF_STRING, IDC_UNDO, L"Undo\tCtrl+Z");
        AppendMenuW(edit, MF_STRING, IDC_REDO, L"Redo\tCtrl+Shift+Z");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"Edit");

        HMENU track = CreatePopupMenu();
        AppendMenuW(track, MF_STRING, IDC_ADDTRACK, L"Add Track");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)track, L"Track");

        HMENU help = CreatePopupMenu();
        AppendMenuW(help, MF_STRING, IDC_CONTROLS, L"Controls\u2026");
        AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"Help");

        SetMenu(hwnd, bar);
    }

    void onCommand(int id) {
        switch (id) {
        case IDC_ADDFILES: addFiles(); break;
        case IDC_OPENPROJ: openProjectFile(); break;
        case IDC_SAVEPROJ: saveProjectFile(); break;
        case IDC_SAVEPROJAS: saveProjectAs(); break;
        case IDC_EXPORTMIX: exportMix(); break;
        case IDC_EXIT: DestroyWindow(hwnd); break;
        case IDC_UNDO: doUndo(); break;
        case IDC_REDO: doRedo(); break;
        case IDC_ADDTRACK: doc.addTrack(); afterPlaceRefresh(); break;
        case IDC_CONTROLS: showControls(); break;
        }
    }

    // --------------------------------------------------------- keyboard
    void onKey(WPARAM k) {
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        bool shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (ctrl && (k == 'Z')) { if (shift) doRedo(); else doUndo(); return; }
        if (ctrl && (k == 'Y')) { doRedo(); return; }
        if (ctrl && (k == 'S')) { saveProjectFile(); return; }
        if (ctrl && (k == 'O')) { addFiles(); return; }
        if (k == VK_SPACE) {
            if (previewClipId >= 0 && !timelinePlaying) togglePlayClip(previewClipId);
            else playAll();
            return;
        }
        if (k == VK_DELETE && selClipId < 0 && previewClipId >= 0) { }
    }

    void onWheel(POINT p, int delta, bool ctrl, bool shift) {
        ScreenToClient(hwnd, &p);
        if (PtInRect(&rcLibrary, p)) {
            libScroll -= delta / 2; clampScroll(); refresh(); return;
        }
        if (PtInRect(&rcTimeline, p)) {
            if (ctrl) {
                double old = pxPerSec;
                pxPerSec *= (delta > 0 ? 1.15 : 1 / 1.15);
                pxPerSec = std::max(8.0, std::min(2000.0, pxPerSec));
                (void)old;
            } else if (shift) {
                tlScrollY -= delta / 2;
            } else {
                tlScrollX -= delta;
            }
            clampScroll(); refresh();
        }
    }

    // --------------------------------------------------------- timer / playend
    void onTimer() {
        if (engine.isPlaying()) {
            if (timelinePlaying) playheadFrame = engine.position();
            else if (previewClipId >= 0) {
                int64_t pos = engine.position();
                previewCursor = previewIsSel ? (selStart + pos) : pos;
            }
            refresh();
        }
    }
    void onPlayEnd() {
        // source drained
        if (timelinePlaying) { timelinePlaying = false; }
        else if (previewClipId >= 0) {
            const Clip* c = doc.project().findClip(previewClipId);
            previewCursor = previewIsSel ? selEnd : (c ? c->frames() : 0);
        }
        refresh();
    }
};

// ----------------------------------------------------------------- window plumbing
static App* g_app = nullptr;

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* a = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        auto* cs = (CREATESTRUCTW*)lp;
        a = (App*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)a);
        a->hwnd = hwnd;
        UINT dpi = GetDpiForWindow(hwnd);
        a->sc = dpi / 96.0f;
        a->makeFonts();
        a->buildMenu();
        a->setTitle();
        a->computeLayout();
        SetTimer(hwnd, 1, 33, nullptr);
        return 0;
    }
    case WM_SIZE: a->computeLayout(); a->clampScroll(); a->refresh(); return 0;
    case WM_DPICHANGED: {
        a->sc = LOWORD(wp) / 96.0f; a->makeFonts();
        RECT* pr = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, pr->left, pr->top, pr->right - pr->left, pr->bottom - pr->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        a->computeLayout(); a->refresh(); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        a->computeLayout();
        a->paint(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: a->onLDown({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, false); return 0;
    case WM_LBUTTONDBLCLK: a->onLDown({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, true); return 0;
    case WM_MOUSEMOVE: a->onMouseMove({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0;
    case WM_LBUTTONUP: a->onLUp({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0;
    case WM_RBUTTONDOWN: a->onRDown({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }); return 0;
    case WM_MOUSEWHEEL:
        a->onWheel({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, GET_WHEEL_DELTA_WPARAM(wp),
                   (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0,
                   (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0);
        return 0;
    case WM_KEYDOWN: a->onKey(wp); return 0;
    case WM_COMMAND: if (HIWORD(wp) == 0 && lp == 0) { a->onCommand(LOWORD(wp)); return 0; } break;
    case WM_TIMER: a->onTimer(); return 0;
    case WM_APP_PLAYEND: a->onPlayEnd(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int runApp(HINSTANCE hInst, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    if (!mfio::startup()) { MessageBoxW(nullptr, L"Media Foundation failed to start.", L"Error", MB_ICONERROR); return 1; }

    App app;
    std::wstring engErr;
    if (!app.engine.init(&engErr))
        MessageBoxW(nullptr, (L"Audio output unavailable: " + engErr + L"\nPlayback will be silent.").c_str(),
                    L"Audio warning", MB_ICONWARNING);
    app.rate = app.engine.sampleRate() > 0 ? app.engine.sampleRate() : 48000;
    app.doc.init(app.rate);

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AudioClipEditorMain";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Audio Clip Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 800,
        nullptr, nullptr, hInst, &app);
    g_app = &app;

    // engine end-of-source -> post to UI thread
    app.engine.setEndCallback([hwnd] { PostMessageW(hwnd, WM_APP_PLAYEND, 0, 0); });

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    app.engine.shutdown();
    mfio::shutdown();
    return 0;
}
