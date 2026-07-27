#include "app.h"
#include "document.h"
#include "engine.h"
#include "decoder.h"
#include "encoder.h"
#include "waveform.h"
#include "dialogs.h"
#include "dsp.h"
#include "layout.h"
#include "transport.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <string>
#include <vector>
#include <algorithm>
#include <climits>

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
    IDM_EDIT, IDM_RENAME, IDM_DELETE,
    IDM_NORM_MATCH, IDM_NORM_ALL, IDM_DENOISE, IDM_DENOISE_SEL, IDM_DENOISE_ALL,
    IDM_GETPROFILE, IDM_GETPROFILE_CLIP,
    IDM_VOICEISO, IDM_VOICEISO_SEL, IDM_VOICEISO_ALL,
    IDM_EXPORTCLIP, IDM_EXPORTSEL,
    IDM_SILENCESEL, IDM_DELETESEL,
    IDM_ADDTL_BASE = 200,   // + track index
    IDM_TL_REMOVE = 300, IDM_TL_REMOVE_TRACK,
    IDM_TRK_DENOISE = 320, IDM_TRK_RENAME, IDM_TRK_REMOVE, IDM_TRK_VOICEISO,
    IDM_SORT_NAME = 340, IDM_SORT_TIME,
    IDM_REDO_BASE = 400,
    IDM_APPLYCAP_BASE = 500   // + recent-capture index (apply to the clicked clip)
};

// Menu bar command ids
enum {
    IDC_ADDFILES = 1000, IDC_OPENPROJ, IDC_SAVEPROJ, IDC_SAVEPROJAS,
    IDC_EXPORTMIX, IDC_EXIT,
    IDC_UNDO, IDC_REDO, IDC_ADDTRACK, IDC_CONTROLS
};

struct CardLayout { int clipId; RECT card, top, play, del, wave, vol, crop, savesel; };
struct PlacedLayout { int trackId, index, clipId; RECT rc; };
struct TrackLayout { int trackId; RECT header, lane; RECT nameRc, delRc, volRc; };

// A remembered background-noise capture. The computed dsp::NoiseProfile is the
// per-capture "processing result" (per-bin noise power means) that is reused
// unchanged for every clip the capture is applied to, so we remember it here
// rather than recomputing when the user picks the capture again from recents.
struct NoiseCapture { dsp::NoiseProfile profile; std::wstring desc; };

enum class Mode { None, WaveSelect, CardDrag, ClipMove, TimelineSeek, ClipVolume, TrackVolume, ZoomDrag, TlVScroll };

struct App {
    HWND hwnd = nullptr;
    Document doc;
    PlaybackEngine engine;
    int rate = 48000;
    float sc = 1.0f;               // dpi scale
    HFONT fNorm=0, fSmall=0, fBold=0, fBig=0;

    RECT rcTransport{}, rcLibrary{}, rcTimeline{};
    RECT tbRects[TB_COUNT]{};
    RECT zoomRc{};                 // global time-scale slider (transport bar)

    std::wstring projectPath;      // current .acep path (empty = unsaved)
    bool lastTitleDirty = false;   // last unsaved-changes state reflected in the title

    // scroll / zoom
    int libScroll = 0, libContentH = 0;
    double pxPerSec = 90.0;
    int tlScrollX = 0, tlScrollY = 0, tlContentH = 0;
    int trackHeaderW = 128, rulerH = 22;
    int vscrollGrab = 0;           // grab offset within the timeline scrollbar thumb

    // layout caches (rebuilt each layout())
    std::vector<CardLayout> cards;
    std::vector<PlacedLayout> placed;
    std::vector<TrackLayout> trackLays;

    // playback / preview state.
    // previewCursor is the authoritative play position for the previewed clip: the
    // yellow cursor is drawn there, and play/resume always (re)starts there, so a
    // seek made while stopped or paused is honoured.
    int previewClipId = -1;
    bool previewIsSel = false;     // the armed source spans the selection, not the whole clip
    int64_t previewCursor = 0;     // frame within clip (display + start point)
    int64_t previewBegin = 0;      // frame range the armed source covers, [begin,end)
    int64_t previewEnd = 0;
    bool previewSeekPending = false;  // cursor moved while not playing; re-arm on resume
    bool timelinePlaying = false;
    int64_t playheadFrame = 0;

    // selection (belongs to selClipId)
    int selClipId = -1;
    int64_t selStart = 0, selEnd = 0;
    int64_t waveAnchor = 0;        // fixed edge while sweeping/edge-dragging a card selection
    bool waveEdgeDrag = false;     // true when a card WaveSelect drag grabbed an existing edge

    // The selection as it stood when the current mouse gesture began, so Esc can
    // put it back (see cancelDrag). Captured on every button-press rather than
    // only on the presses that start a selection drag: a press cannot always tell
    // yet what it will turn into -- a sweep on a card becomes a drag-to-timeline
    // if the pointer leaves the library, and that conversion clears the selection.
    int selSaveClipId = -1;
    int64_t selSaveStart = 0, selSaveEnd = 0;

    // voice cleaner session state: last-used options + captured noise profile
    // (Audacity-style; profile lives for the session, like Audacity's)
    dsp::NROptions nrOpts;
    dsp::NoiseProfile noiseProfile;        // the "active" capture (used by the dialog)
    std::wstring noiseProfileDesc;
    std::vector<NoiseCapture> noiseCaptures;  // recent captures, most-recent first

    // "Remove non-voice" (voice isolation) options, persisted for the session
    dsp::VoiceIsolateOptions viOpts;

    // interaction
    Mode mode = Mode::None;
    POINT downPt{};
    bool dragged = false;
    int dragClipId = -1;           // CardDrag / ClipMove source clip
    int volTrackId = -1;           // TrackVolume drag target
    int moveTrackId = -1, moveIndex = -1;
    int64_t moveGrabOffset = 0;    // frames from clip start to grab point
    int hotTB = -1;
    int hotSelClip = -1;           // card whose selection-action button is hovered
    int hotSelBtn = 0;             // 0 none, 1 crop, 2 save-selection

    // full-window clip editor
    int editClipId = -1;           // >=0 => editor overlay is active for this clip
    bool editFineToggle = false;   // user forced the fine-tune edge strips on
    int editDrag = 0;              // 0 none, 1 main-select, 2 left strip, 3 right strip
    int64_t stripAnchorStart = 0;  // window start snapshot while dragging a strip
    int64_t stripSpanFrames = 0;   // width (frames) shown by each fine-tune strip
    int64_t stripFixedEdge = 0;    // the opposite (non-dragged) selection edge, captured at drag start
    int64_t stripDragFrame = 0;    // frame under the cursor for the strip currently being dragged
    int64_t mainDragAnchor = 0;    // fixed anchor frame while sweeping a selection on the main waveform
    bool mainEdgeDrag = false;     // true when an editor main-view drag grabbed an existing selection edge
    int edHot = 0;                 // hovered editor button (see EB_* below)
    // editor layout rects (rebuilt in computeEditorLayout)
    RECT edMain{}, edRuler{}, edLeft{}, edRight{};
    enum { EB_NONE, EB_PLAY, EB_PLAYSEL, EB_FINE, EB_CROP, EB_SILENCE, EB_DELSEL, EB_SAVE,
           EB_CAPTURE, EB_CLEAR, EB_DONE, EB_COUNT };
    RECT edBtn[EB_COUNT]{};
    int edToolbarH = 0;            // computed in computeEditorLayout; the row wraps when narrow
    bool timerFast = false;        // timer is at playhead-animation rate (see setTimerRate)

    // ------------------------------------------------------------- helpers
    int S(int v) const { return (int)(v * sc + 0.5f); }
    bool hasSel() const { return selClipId >= 0 && selEnd > selStart; }
    // Play All has something to play only when clips are on the timeline.
    bool timelineHasContent() const { return doc.project().timelineLengthFrames() > 0; }

    // How close (px, each side) the cursor must be to a selection edge to grab it.
    // Kept generous so the edge is easy to click without a pixel-perfect aim.
    int selEdgeGrab() const { return S(8); }
    // Which edge of a selection drawn at [sx0, sx1] the cursor at x is grabbing:
    // 0 = neither, 1 = left/start edge, 2 = right/end edge. When both are in range
    // (a tiny selection) the nearer edge wins.
    int hitSelEdge(int x, int sx0, int sx1) const {
        int g = selEdgeGrab();
        int dl = std::abs(x - sx0), dr = std::abs(x - sx1);
        bool nl = dl <= g, nr = dr <= g;
        if (nl && nr) return dl <= dr ? 1 : 2;
        if (nl) return 1;
        if (nr) return 2;
        return 0;
    }
    // Is an edge-drag currently in progress? (used to keep the resize cursor.)
    bool draggingSelEdge() const {
        return editDrag == 2 || editDrag == 3 || mainEdgeDrag ||
               (mode == Mode::WaveSelect && waveEdgeDrag);
    }
    // Would a click at p grab a selection edge (card wave, editor main view, or a
    // fine-tune strip)? Drives the horizontal-resize cursor hint.
    bool overSelEdge(POINT p) const {
        if (!hasSel()) return false;
        if (editorActive()) {
            if (selClipId != editClipId) return false;
            if (editFineNeeded() && (PtInRect(&edLeft, p) || PtInRect(&edRight, p))) return true;
            if (PtInRect(&edMain, p)) return hitSelEdge(p.x, edMainX(selStart), edMainX(selEnd)) != 0;
            return false;
        }
        if (!PtInRect(&rcLibrary, p)) return false;
        for (auto& c : cards) if (c.clipId == selClipId) {
            if (!PtInRect(&c.wave, p)) return false;
            const Clip* cl = doc.project().findClip(selClipId);
            int64_t nf = cl ? cl->frames() : 0;
            if (nf <= 0) return false;
            int ww = std::max(1, (int)(c.wave.right - c.wave.left));
            int sx0 = c.wave.left + (int)((double)selStart / nf * ww);
            int sx1 = c.wave.left + (int)((double)selEnd / nf * ww);
            return hitSelEdge(p.x, sx0, sx1) != 0;
        }
        return false;
    }
    // A card is being dragged to reorder / place it.
    bool draggingCard() const { return mode == Mode::CardDrag && dragged; }
    // Is p over a card's title-bar drag handle (the grab area for reordering /
    // dragging to a track), excluding the play / delete buttons that sit in it?
    bool overCardDragHandle(POINT p) const {
        if (editorActive() || !PtInRect(&rcLibrary, p)) return false;
        for (auto& c : cards) if (PtInRect(&c.top, p)) {
            if (PtInRect(&c.play, p) || PtInRect(&c.del, p)) return false;
            return true;
        }
        return false;
    }

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

    // Global time scale (pixels per second) bounds + log mapping for the slider.
    static constexpr double kZoomMin = 8.0, kZoomMax = 2000.0;
    float zoomToFrac() const {
        double lo = std::log(kZoomMin), hi = std::log(kZoomMax);
        return (float)((std::log(pxPerSec) - lo) / (hi - lo));
    }
    void setZoomFrac(float f) {
        f = std::max(0.0f, std::min(1.0f, f));
        double lo = std::log(kZoomMin), hi = std::log(kZoomMax);
        pxPerSec = std::exp(lo + f * (hi - lo));
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

    // ----------------------------------------------------- full-window clip editor
    bool editorActive() const { return editClipId >= 0; }
    int64_t editClipFrames() const {
        const Clip* c = doc.project().findClip(editClipId);
        return c ? c->frames() : 0;
    }
    void openClipEditor(int clipId) {
        if (!doc.project().findClip(clipId)) return;
        editClipId = clipId;
        editDrag = 0; editFineToggle = false; edHot = EB_NONE;
        if (selClipId != clipId) { selClipId = clipId; selStart = selEnd = 0; }
        int64_t cf = editClipFrames();
        stripSpanFrames = std::min<int64_t>(std::max<int64_t>(1, cf), (int64_t)(rate * 0.3));
        computeLayout();
        refresh();
    }
    void closeClipEditor() { editClipId = -1; editDrag = 0; refresh(); }

    // Are the fine-tune strips applicable to this clip at all (toggle on, or the
    // main view too coarse to nudge precisely by hand)? Independent of selection.
    bool editFineApplicable() const {
        if (selClipId != editClipId) return false;
        if (editFineToggle) return true;
        int64_t cf = editClipFrames();
        if (cf <= 0) return false;
        double durSec = (double)cf / rate;
        int w = std::max(1, (int)(edMain.right - edMain.left));
        double pxPerSecMain = w / std::max(0.001, durSec);
        return pxPerSecMain < 100.0;
    }
    // does the editor currently need the fine-tune strips?
    bool editFineNeeded() const {
        if (!editFineApplicable()) return false;
        // Keep the strips reserved while dragging a main-waveform edge so they
        // stay put and slide to follow the moving selection instead of vanishing.
        if (editDrag == 1) return true;
        return hasSel() && selClipId == editClipId;
    }

    int edMainX(int64_t f) const {
        int64_t cf = std::max<int64_t>(1, editClipFrames());
        int w = (int)(edMain.right - edMain.left);
        return edMain.left + (int)((double)f / cf * w);
    }
    int64_t edMainFrame(int x) const {
        int64_t cf = std::max<int64_t>(1, editClipFrames());
        int w = std::max(1, (int)(edMain.right - edMain.left));
        double t = (double)(x - edMain.left) / w;
        t = std::max(0.0, std::min(1.0, t));
        return (int64_t)(t * cf);
    }
    // window (frames) shown by a fine strip centred on `center`
    void stripWindow(int64_t center, int64_t& ws, int64_t& we) const {
        int64_t cf = editClipFrames();
        int64_t span = std::min<int64_t>(std::max<int64_t>(1, stripSpanFrames), std::max<int64_t>(1, cf));
        ws = center - span / 2; we = ws + span;
        if (ws < 0) { ws = 0; we = span; }
        if (we > cf) { we = cf; ws = std::max<int64_t>(0, cf - span); }
    }

    // ----- editor interaction ------------------------------------------------
    void edButton(int id) {
        switch (id) {
        // Each transport button owns its own range: it pauses only what it
        // started, and switches playback to itself when the other one is running.
        case EB_PLAY: togglePreview(editClipId, false, true); break;   // whole clip
        case EB_PLAYSEL: if (hasSel() && selClipId == editClipId) togglePreview(editClipId, true, true); break;
        case EB_FINE: editFineToggle = !editFineToggle; break;
        case EB_CROP: if (hasSel() && selClipId == editClipId) cropSelection(); break;
        case EB_SILENCE: removeSelectedRange(editClipId, true); break;
        case EB_DELSEL: removeSelectedRange(editClipId, false); break;
        case EB_SAVE: if (hasSel() && selClipId == editClipId) saveSelectionAsClip(); break;
        case EB_CAPTURE: captureNoiseProfileFromEditor(); break;
        case EB_CLEAR: selClipId = editClipId; selStart = selEnd = 0; break;
        case EB_DONE: closeClipEditor(); return;
        }
        refresh();
    }
    void updateStripEdge(const RECT& box, int x) {
        int64_t cf = editClipFrames();
        int64_t span = std::min<int64_t>(std::max<int64_t>(1, stripSpanFrames), std::max<int64_t>(1, cf));
        int w = std::max(1, (int)(box.right - box.left));
        double t = (double)(x - box.left) / w; t = std::max(0.0, std::min(1.0, t));
        int64_t f = stripAnchorStart + (int64_t)(t * span);
        f = std::max<int64_t>(0, std::min<int64_t>(cf, f));
        stripDragFrame = f;
        // Keep a valid (non-collapsing) selection even if the dragged edge crosses
        // the fixed one: the selection is simply the span between the two.
        selStart = std::min(f, stripFixedEdge);
        selEnd   = std::max(f, stripFixedEdge);
    }
    void beginStripDrag(int which, int64_t edge, const RECT& box, int x) {
        editDrag = which;
        stripFixedEdge = (which == 2) ? selEnd : selStart;  // the edge we are NOT dragging
        int64_t ws, we; stripWindow(edge, ws, we); stripAnchorStart = ws;
        updateStripEdge(box, x);
        SetCapture(hwnd); refresh();
    }
    void edDown(POINT p, bool /*dbl*/) {
        SetFocus(hwnd);
        downPt = p; dragged = false;
        for (int i = EB_PLAY; i < EB_COUNT; ++i)
            if (PtInRect(&edBtn[i], p)) { edButton(i); return; }
        bool sel = hasSel() && selClipId == editClipId;
        if (editFineNeeded() && sel) {
            if (PtInRect(&edLeft, p))  { beginStripDrag(2, selStart, edLeft, p.x);  return; }
            if (PtInRect(&edRight, p)) { beginStripDrag(3, selEnd, edRight, p.x);    return; }
        }
        if (PtInRect(&edMain, p)) {
            int64_t f = edMainFrame(p.x);
            // Grab an existing selection edge (if the cursor is near one) so the
            // user can nudge one side without redrawing the whole selection.
            mainEdgeDrag = false;
            if (hasSel() && selClipId == editClipId) {
                int edge = hitSelEdge(p.x, edMainX(selStart), edMainX(selEnd));
                if (edge == 1)      { mainDragAnchor = selEnd;   mainEdgeDrag = true; }
                else if (edge == 2) { mainDragAnchor = selStart; mainEdgeDrag = true; }
            }
            editDrag = 1; selClipId = editClipId;
            if (!mainEdgeDrag) { mainDragAnchor = f; selStart = selEnd = f; }
            SetCapture(hwnd); refresh();
        }
    }
    void edMove(POINT p) {
        int oldHot = edHot; edHot = EB_NONE;
        for (int i = EB_PLAY; i < EB_COUNT; ++i) if (PtInRect(&edBtn[i], p)) edHot = i;
        if (editDrag == 0) { if (edHot != oldHot) refresh(); return; }
        if (!dragged && (std::abs(p.x - downPt.x) > S(3) || std::abs(p.y - downPt.y) > S(3))) dragged = true;
        if (editDrag == 1) {
            // Keep a valid selection even when the cursor sweeps past the anchor,
            // so the highlight (and strips) never blink out mid-drag.
            int64_t f = edMainFrame(p.x);
            selStart = std::min(mainDragAnchor, f);
            selEnd   = std::max(mainDragAnchor, f);
        }
        else if (editDrag == 2) updateStripEdge(edLeft, p.x);
        else if (editDrag == 3) updateStripEdge(edRight, p.x);
        refresh();
    }
    void edUp(POINT p) {
        if (GetCapture() == hwnd) ReleaseCapture();
        int d = editDrag; editDrag = 0;
        if (d == 1) {
            if (!dragged && !mainEdgeDrag) { selClipId = editClipId; selStart = selEnd = 0; seekClip(editClipId, edMainFrame(p.x)); }
            // (selStart/selEnd stay sorted during the sweep, so no swap needed; a
            //  no-move edge grab simply leaves the selection unchanged.)
        }
        mainEdgeDrag = false;
        refresh();
    }
    void edWheel(POINT p, int delta) {
        if (!PtInRect(&edLeft, p) && !PtInRect(&edRight, p)) return;
        int64_t cf = std::max<int64_t>(1, editClipFrames());
        double f = (delta > 0 ? 1.0 / 1.2 : 1.2);
        stripSpanFrames = (int64_t)(stripSpanFrames * f);
        stripSpanFrames = std::max<int64_t>((int64_t)(rate * 0.02), std::min<int64_t>(cf, stripSpanFrames));
        refresh();
    }

    // --------------------------------------------------------- layout
    int trackLaneH() const { return S(72); }
    int trackGap()   const { return S(6); }

    void computeLayout() {
        RECT rc; GetClientRect(hwnd, &rc);
        int tH = S(62);
        rcTransport = { 0, 0, rc.right, tH };
        trackHeaderW = S(128);
        rulerH = S(22);

        // Tracks view on top, sized to just fit the current number of tracks;
        // the clip library fills the remaining space below it.
        int nTracks = (int)doc.project().tracks.size();
        int laneH = trackLaneH(), gap = trackGap();
        int neededTL = rulerH + nTracks * (laneH + gap) + gap;
        int availBelow = rc.bottom - tH;
        int minLib = S(150);
        int tlH = neededTL;
        if (tlH > availBelow - minLib) tlH = availBelow - minLib;   // keep library visible
        int floorTL = rulerH + gap;
        if (tlH < floorTL) tlH = std::min(floorTL, availBelow);     // sanity floor
        rcTimeline = { 0, tH, rc.right, tH + tlH };
        rcLibrary  = { 0, tH + tlH, rc.right, rc.bottom };

        layoutTransport();
        layoutTracks();
        layoutCards();
        if (editorActive()) computeEditorLayout(rc);
    }

    void computeEditorLayout(const RECT& rc) {
        int pad = S(16);
        // Toolbar buttons, left to right, wrapping to a second row when they don't
        // fit. They no longer fit on one row at the default window size above
        // 125% DPI, and a button that runs off the edge is simply unreachable, so
        // the toolbar grows downwards instead and the waveform below starts lower.
        const int by = S(10), bh = S(34), gap = S(8), doneW = S(96);
        // Reserve room for Done, which is pinned to the right of the final row.
        const int avail = std::max(S(200), (int)rc.right - pad - doneW - S(16));
        // Order matches the enum below; the negative entry is the group gap that
        // separates transport from the edit actions.
        static const int order[] = { EB_PLAY, EB_PLAYSEL, EB_FINE, EB_NONE, EB_CROP,
                                     EB_SILENCE, EB_DELSEL, EB_SAVE, EB_CAPTURE, EB_CLEAR };
        // EB_PLAYSEL is sized for its *widest* label, "Pause selection", so the
        // button doesn't have to resize (and re-flow the whole toolbar under the
        // cursor) the moment playback starts.
        const std::vector<int> widths = { S(96), S(140), S(150), -S(16), S(120),
                                          S(130), S(130), S(150), S(150), S(120) };
        auto f = layout::flowButtons(widths, pad, by, bh, gap, avail, S(10));
        for (size_t i = 0; i < widths.size(); ++i)
            if (order[i] != EB_NONE) edBtn[order[i]] = f.rects[i];
        // Done on the far right of the final row, but never overlapping the group:
        // if even that row is too narrow, park it right after the last button.
        int doneLeft = std::max(f.endX, (int)rc.right - pad - doneW);
        edBtn[EB_DONE] = { doneLeft, f.endY, doneLeft + doneW, f.endY + bh };
        // Floor at the original single-row height so an unwrapped toolbar lays
        // out exactly as it did before wrapping existed.
        edToolbarH = std::max(f.height, S(56));

        int topH = edToolbarH;
        int contentTop = topH + pad;
        int contentBot = rc.bottom - pad;
        int rulerH2 = S(18);
        // main waveform spans the full content width (strips stack below it, so the
        // main width is independent of whether strips show) — set it first so
        // editFineNeeded() can measure the main scale.
        edMain = { pad, contentTop + rulerH2, rc.right - pad, contentBot };
        bool strips = editFineNeeded();
        int stripH = strips ? S(150) : 0;
        int mainBot = contentBot - (strips ? stripH + pad : 0);
        edRuler = { pad, contentTop, rc.right - pad, contentTop + rulerH2 };
        edMain  = { pad, contentTop + rulerH2, rc.right - pad, mainBot };
        if (strips) {
            int mid = rc.right / 2;
            edLeft  = { pad, contentBot - stripH, mid - S(8), contentBot };
            edRight = { mid + S(8), contentBot - stripH, rc.right - pad, contentBot };
        } else { edLeft = edRight = RECT{}; }
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
        tbRects[TB_UNDO] = { rx - S(70), y, rx, y + h }; rx -= S(78);
        // global time-scale slider (with a "Scale" label drawn to its left)
        int zsW = S(120);
        zoomRc = { rx - zsW, y + h / 2 - S(7), rx, y + h / 2 + S(7) };
    }

    void layoutCards() {
        cards.clear();
        const auto& lib = doc.project().library;
        int pad = S(12);
        int cardH = S(120);
        // Card widths are proportional to clip duration at the shared timeline
        // scale (pxPerSec), so a clip is drawn the same length here as it is on a
        // track. Floored so the controls stay usable, capped to the viewport.
        int minW = S(120);
        int maxRight = rcLibrary.right - pad;
        int maxW = std::max(minW, (int)(rcLibrary.right - rcLibrary.left) - 2 * pad);
        int x = rcLibrary.left + pad;
        int y = rcLibrary.top + pad - libScroll;
        int rowH = cardH + pad;
        int rows = lib.empty() ? 0 : 1;
        for (const auto& c : lib) {
            int cardW = (int)(c.durationSec() * pxPerSec + 0.5);
            cardW = std::max(minW, std::min(cardW, maxW));
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
            // Selection-action buttons overlaid at the top-right of the waveform;
            // only drawn / hit-tested while this clip has an active selection.
            { int bh = S(17), by0 = cl.wave.top + S(3);
              int savW = S(58), crpW = S(42), inset = S(4), gap2 = S(4);
              int bx1 = cl.wave.right - inset;
              cl.savesel = { bx1 - savW, by0, bx1, by0 + bh };
              cl.crop    = { cl.savesel.left - gap2 - crpW, by0, cl.savesel.left - gap2, by0 + bh };
              if (cl.crop.left < cl.wave.left) cl.crop.left = cl.wave.left; }
            cards.push_back(cl);
            x += cardW + pad;
        }
        // content height for scroll clamp
        libContentH = rows > 0 ? pad + rows * rowH : 0;
    }

    void layoutTracks() {
        trackLays.clear(); placed.clear();
        const auto& tracks = doc.project().tracks;
        int laneH = trackLaneH();
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
            y += laneH + trackGap();
        }
        tlContentH = (y + tlScrollY) - (rcTimeline.top + rulerH) + trackGap();
    }

    // --------------------------------------------------------- painting
    void paint(HDC hdc, const RECT& client) {
        HDC mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, client.right, client.bottom);
        HGDIOBJ oldb = SelectObject(mem, bmp);
        SetBkMode(mem, TRANSPARENT);

        fill(mem, client, col::bg);
        if (editorActive()) {
            paintEditor(mem, client);
        } else {
            paintLibrary(mem);
            paintTimeline(mem);
            paintTransport(mem);
            paintDragOverlay(mem);
        }

        BitBlt(hdc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldb); DeleteObject(bmp); DeleteDC(mem);
    }

    static std::wstring fmtHMSms(double s) {
        if (s < 0) s = 0;
        int mm = (int)(s / 60); double rem = s - mm * 60;
        wchar_t b[32]; swprintf(b, 32, L"%d:%06.3f", mm, rem);
        return b;
    }

    void paintEditor(HDC h, const RECT& client) {
        const Clip* c = doc.project().findClip(editClipId);
        if (!c) { closeClipEditor(); return; }
        int64_t cf = std::max<int64_t>(1, c->frames());
        bool sel = hasSel() && selClipId == editClipId;

        // toolbar
        RECT top = { 0, 0, client.right, edToolbarH };
        fill(h, top, col::transport);
        // Two independent transports, so each button reports its own state. The
        // one that is running shows Pause; the accent marks a transport that can
        // be started, which for "Play selection" means only when a selection
        // exists. (It used to be Play alone that was accented and Play selection
        // that was always grey, even when both were equally clickable -- and a
        // selection audition flipped *Play* to Pause, i.e. a button the user had
        // not pressed.)
        bool playing = previewClipId == editClipId && !timelinePlaying && engine.isPlaying();
        bool playingSel = playing && previewIsSel;
        bool playingAll = playing && !previewIsSel;
        button(h, edBtn[EB_PLAY],    playingAll ? L"\u275A\u275A Pause" : L"\u25B6 Play",
               col::accentDk, col::text, edHot == EB_PLAY, fNorm);
        button(h, edBtn[EB_PLAYSEL], playingSel ? L"\u275A\u275A Pause selection"
                                                : L"\u25B6 Play selection",
               sel ? col::accentDk : col::btn,
               sel ? col::text : col::dim, edHot == EB_PLAYSEL, fNorm);
        bool fineOn = editFineNeeded();
        button(h, edBtn[EB_FINE], fineOn ? L"\u2713 Fine-tune edges" : L"Fine-tune edges",
               fineOn ? col::accentDk : col::btn, col::text, edHot == EB_FINE, fNorm);
        button(h, edBtn[EB_CROP],  L"Crop to selection\u2026", col::btn,
               sel ? col::text : col::dim, edHot == EB_CROP, fNorm);
        button(h, edBtn[EB_SILENCE], L"Silence selection", col::btn,
               sel ? col::text : col::dim, edHot == EB_SILENCE, fNorm);
        button(h, edBtn[EB_DELSEL], L"Delete selection", col::btn,
               sel ? col::text : col::dim, edHot == EB_DELSEL, fNorm);
        button(h, edBtn[EB_SAVE],  L"Save selection as clip", col::btn,
               sel ? col::text : col::dim, edHot == EB_SAVE, fNorm);
        button(h, edBtn[EB_CAPTURE], sel ? L"Capture noise (sel)" : L"Capture noise (clip)",
               col::btn, col::text, edHot == EB_CAPTURE, fNorm);
        button(h, edBtn[EB_CLEAR], L"Clear selection", col::btn,
               sel ? col::text : col::dim, edHot == EB_CLEAR, fNorm);
        button(h, edBtn[EB_DONE],  L"Done", col::btn, col::text, edHot == EB_DONE, fBold);

        // ruler + selection read-out
        fill(h, edRuler, col::ruler);
        std::wstring info = L"Editing: " + c->name + L"    ";
        if (sel) info += L"selection " + fmtHMSms((double)selStart / rate) + L" \u2013 " +
                         fmtHMSms((double)selEnd / rate) +
                         L"  (" + fmtHMSms((double)(selEnd - selStart) / rate) + L")";
        else info += L"drag on the waveform to select";
        RECT infoRc = { edRuler.left + S(6), edRuler.top, edRuler.right - S(6), edRuler.bottom };
        textOut(h, infoRc, info, col::dim, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // main waveform
        fill(h, edMain, col::waveBg);
        if (c->buffer && c->peaks) wf::draw(h, edMain, *c->buffer, *c->peaks, 0, cf, col::wave);
        if (sel) {
            int sx0 = edMainX(selStart), sx1 = edMainX(selEnd);
            RECT sr = { sx0, edMain.top, sx1, edMain.bottom };
            fill(h, sr, col::selRect);
            SaveDC(h); IntersectClipRect(h, sr.left, sr.top, sr.right, sr.bottom);
            if (c->buffer && c->peaks) wf::draw(h, edMain, *c->buffer, *c->peaks, 0, cf, col::waveSel);
            RestoreDC(h, -1);
            drawVLine(h, sx0, edMain.top, edMain.bottom, col::waveSel);
            drawVLine(h, sx1, edMain.top, edMain.bottom, col::waveSel);
        }
        if (previewClipId == editClipId) {
            int cx = edMainX(previewCursor);
            drawVLine(h, cx, edMain.top, edMain.bottom, col::playhead);
        }
        // light second-mark ticks on the ruler
        {
            double durSec = (double)cf / rate;
            double step = 1.0; int w = (int)(edMain.right - edMain.left);
            while (step / durSec * w < S(60)) step *= 2;
            for (double t = 0; t <= durSec; t += step) {
                int x = edMainX((int64_t)(t * rate));
                drawVLine(h, x, edRuler.bottom - S(6), edRuler.bottom, col::cardEdge);
            }
        }

        // fine-tune strips. During a main-edge drag the selection may momentarily
        // be zero-length; still draw the strips (centred on the moving edges) so
        // they slide with the drag rather than blanking out.
        if (fineOn && (sel || editDrag == 1)) {
            // While a strip is being dragged, keep its knob pinned to the cursor
            // frame so it tracks smoothly even if it crosses the fixed edge.
            int64_t leftEdge  = (editDrag == 2) ? stripDragFrame : selStart;
            int64_t rightEdge = (editDrag == 3) ? stripDragFrame : selEnd;
            paintFineStrip(h, edLeft,  L"Start edge", leftEdge,  editDrag == 2, c);
            paintFineStrip(h, edRight, L"End edge",   rightEdge, editDrag == 3, c);
        }
    }

    void paintFineStrip(HDC h, const RECT& box, const wchar_t* label,
                        int64_t edge, bool dragging, const Clip* c) {
        int64_t cf = std::max<int64_t>(1, c->frames());
        int64_t ws, we;
        if (dragging) { ws = stripAnchorStart; we = ws + std::min<int64_t>(std::max<int64_t>(1, stripSpanFrames), cf); }
        else stripWindow(edge, ws, we);
        int64_t span = std::max<int64_t>(1, we - ws);
        RECT lab = { box.left, box.top, box.right, box.top + S(16) };
        fill(h, lab, col::transport);
        std::wstring t = std::wstring(label) + L"   " + fmtHMSms((double)edge / rate) +
                         L"   (\u00b1" + fmtHMSms((double)span / rate / 2) + L" view)";
        textOut(h, lab, L"  " + t, col::dim, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT wv = { box.left, lab.bottom, box.right, box.bottom };
        fill(h, wv, col::waveBg);
        int w = std::max(1, (int)(wv.right - wv.left));
        auto fx = [&](int64_t f) { return wv.left + (int)((double)(f - ws) / span * w); };
        if (c->buffer && c->peaks) wf::draw(h, wv, *c->buffer, *c->peaks, ws, we, col::wave);
        // shade the selected side (left strip: right of edge is inside selection;
        // right strip: left of edge is inside selection)
        // The playhead, when it is inside this strip's window. These strips are
        // the most zoomed-in view in the app -- a few hundred ms across half the
        // window -- so they are exactly where you want to watch the cursor cross
        // an edge you are placing, and they were the one waveform that never
        // drew it. Drawn under the edge marker so the edge stays readable when
        // the two coincide, which is the moment that matters.
        if (previewClipId == editClipId && previewCursor >= ws && previewCursor < we)
            drawVLine(h, fx(previewCursor), wv.top, wv.bottom, col::playhead);
        int ex = fx(edge);
        drawVLine(h, ex, wv.top, wv.bottom, col::waveSel);
        // draggable handle
        RECT knob = { ex - S(5), wv.top, ex + S(5), wv.top + S(10) };
        roundFill(h, knob, col::waveSel, col::waveSel, S(2));
        // outline
        HPEN pen = CreatePen(PS_SOLID, 1, col::cardEdge); HGDIOBJ op = SelectObject(h, pen);
        HGDIOBJ ob = SelectObject(h, GetStockObject(NULL_BRUSH));
        Rectangle(h, box.left, box.top, box.right, box.bottom);
        SelectObject(h, op); SelectObject(h, ob); DeleteObject(pen);
    }

    void drawVLine(HDC h, int x, int y0, int y1, COLORREF c) {
        HPEN pen = CreatePen(PS_SOLID, S(1), c); HGDIOBJ op = SelectObject(h, pen);
        MoveToEx(h, x, y0, nullptr); LineTo(h, x, y1);
        SelectObject(h, op); DeleteObject(pen);
    }

    // Floating ghost of a library clip being dragged toward the timeline. Drawn
    // unclipped so it follows the cursor across panels; once the cursor is over a
    // track lane the in-lane snap preview (in paintTimeline) takes over instead.
    void paintDragOverlay(HDC h) {
        if (mode != Mode::CardDrag || !dragged) return;
        POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
        int tid; if (trackAtPoint(p, tid)) return;   // snapping into a lane takes over
        const Clip* c = doc.project().findClip(dragClipId);
        if (!c) return;
        int w = S(150), ht = S(30);
        RECT g = { p.x - w / 2, p.y - ht / 2, p.x + w / 2, p.y + ht / 2 };
        roundFill(h, g, col::accentDk, col::text, S(6));
        RECT nm = { g.left + S(8), g.top, g.right - S(8), g.bottom };
        textOut(h, nm, c->name, col::text, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

    // Draw the waveform selection over a rect that shows the clip's *whole*
    // buffer spread across [wv.left, wv.right): a tinted band, the waveform
    // redrawn in the accent colour inside it, and an edge line each side.
    //
    // Shared by the library card and by the clip's block on a track lane. A
    // selection belongs to the clip, not to the view that made it, so every place
    // the clip is shown has to show it -- a selection made on a card used to be
    // invisible on that same clip's placement in the timeline, which made the two
    // look like unrelated pieces of audio.
    //
    // `wv` may extend past the visible area (a placed clip scrolled half off the
    // lane), so callers clip; the frame->x mapping deliberately uses the full
    // untrimmed rect so it agrees with the wf::draw underneath.
    void drawSelOverlay(HDC h, const RECT& wv, const Clip& c) {
        if (!c.buffer || !c.peaks) return;
        const int64_t nf = std::max<int64_t>(1, c.frames());
        const int ww = wv.right - wv.left;
        if (ww <= 0) return;
        int sx0 = wv.left + (int)((double)selStart / nf * ww);
        int sx1 = wv.left + (int)((double)selEnd / nf * ww);
        if (sx1 <= sx0) sx1 = sx0 + 1;   // a sub-pixel selection must still show
        RECT sr = { sx0, wv.top, sx1, wv.bottom };
        fill(h, sr, col::selRect);
        SaveDC(h); IntersectClipRect(h, sr.left, sr.top, sr.right, sr.bottom);
        wf::draw(h, wv, *c.buffer, *c.peaks, 0, c.frames(), col::waveSel);
        RestoreDC(h, -1);
        HPEN pen = CreatePen(PS_SOLID, 1, col::waveSel); HGDIOBJ op = SelectObject(h, pen);
        MoveToEx(h, sx0, wv.top, nullptr); LineTo(h, sx0, wv.bottom);
        MoveToEx(h, sx1, wv.top, nullptr); LineTo(h, sx1, wv.bottom);
        SelectObject(h, op); DeleteObject(pen);
    }

    // Hover highlight: lighten the button's own colour so coloured buttons
    // (green Play All, red Stop, accent Crop/Save) stay their colour on hover
    // instead of turning grey.
    static COLORREF lighten(COLORREF c, int amt) {
        int r = std::min(255, GetRValue(c) + amt);
        int g = std::min(255, GetGValue(c) + amt);
        int b = std::min(255, GetBValue(c) + amt);
        return RGB(r, g, b);
    }
    void button(HDC h, const RECT& r, const std::wstring& label, COLORREF bg, COLORREF fg,
                bool hot, HFONT f) {
        roundFill(h, r, hot ? lighten(bg, 22) : bg, col::cardEdge, S(6));
        textOut(h, r, label, fg, f, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Volume slider: track range is linear gain 0..2 (1.0 at the midpoint).
    static float gainToFrac(float g) { float f = g * 0.5f; return f < 0 ? 0 : (f > 1 ? 1 : f); }
    float sliderGainAt(const RECT& r, int x) const {
        float f = (float)(x - r.left) / std::max(1, (int)(r.right - r.left));
        f = std::max(0.0f, std::min(1.0f, f));
        if (std::fabs(f - 0.5f) < 0.04f) f = 0.5f;   // snap to 100% (gain 1.0)
        return f * 2.0f;
    }
    // Slightly expanded hit rect so the thin slider is easy to grab.
    RECT sliderHit(const RECT& r) const { return { r.left - S(6), r.top - S(6), r.right + S(6), r.bottom + S(6) }; }

    void drawSlider(HDC h, const RECT& r, float gain) { drawSliderFrac(h, r, gainToFrac(gain)); }
    void drawSliderFrac(HDC h, const RECT& r, float frac) {
        int cy = (r.top + r.bottom) / 2;
        RECT trk = { r.left, cy - S(2), r.right, cy + S(2) };
        roundFill(h, trk, col::btn, col::cardEdge, S(2));
        frac = std::max(0.0f, std::min(1.0f, frac));
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
        bool canPlayAll = tlPlaying || timelineHasContent();
        if (canPlayAll)
            button(h, tbRects[TB_PLAYALL], pa, col::accentDk, col::text, hotTB == TB_PLAYALL, fBig);
        else   // nothing on the timeline yet -> show disabled rather than a dead green button
            button(h, tbRects[TB_PLAYALL], pa, col::btn, col::dim, false, fBig);
        button(h, tbRects[TB_STOP], L"\u25A0 Stop", col::btn, col::text, hotTB == TB_STOP, fNorm);

        // time readout
        double posSec, totSec;
        if (timelinePlaying) { posSec = (double)playheadFrame / rate; totSec = (double)doc.project().timelineLengthFrames() / rate; }
        else if (previewClipId >= 0) {
            posSec = (double)previewCursor / rate;
            const Clip* c = doc.project().findClip(previewClipId);
            totSec = c ? c->durationSec() : 0;
        } else { posSec = (double)playheadFrame / rate; totSec = (double)doc.project().timelineLengthFrames() / rate; }
        // global time-scale slider ("Scale" label + slider)
        RECT zlab = { zoomRc.left - S(46), rcTransport.top, zoomRc.left - S(4), rcTransport.bottom };
        textOut(h, zlab, L"Scale", col::dim, fSmall, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        drawSliderFrac(h, zoomRc, zoomToFrac());

        RECT tr = { tbRects[TB_STOP].right + S(16), rcTransport.top, zlab.left - S(8), rcTransport.bottom };
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
                          L"Drag a clip up onto a track to place it.",
                    col::dim, fNorm, DT_CENTER | DT_VCENTER);
        }

        for (const auto& cl : cards) {
            const Clip* c = doc.project().findClip(cl.clipId);
            if (!c) continue;
            bool selHere = (selClipId == cl.clipId);
            bool dragThis = (mode == Mode::CardDrag && dragged && cl.clipId == dragClipId);
            roundFill(h, cl.card, selHere ? col::cardSel : col::card,
                      dragThis ? col::accent : col::cardEdge, S(8));

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
            if (c->buffer && c->peaks) {
                int64_t nf = c->frames();
                wf::draw(h, cl.wave, *c->buffer, *c->peaks, 0, nf, col::wave);
                // selection overlay
                if (selectionCovers(cl.clipId)) drawSelOverlay(h, cl.wave, *c);
                // preview cursor line
                if (previewClipId == cl.clipId) {
                    int wl = cl.wave.left, ww = cl.wave.right - cl.wave.left;
                    int cx = wl + (int)((double)previewCursor / std::max<int64_t>(1, nf) * ww);
                    HPEN pen = CreatePen(PS_SOLID, S(1), col::playhead); HGDIOBJ op = SelectObject(h, pen);
                    MoveToEx(h, cx, cl.wave.top, nullptr); LineTo(h, cx, cl.wave.bottom);
                    SelectObject(h, op); DeleteObject(pen);
                }
            }
            // selection-action buttons (crop / save selection as new clip)
            if (hasSel() && selClipId == cl.clipId) {
                button(h, cl.crop, L"Crop", col::accentDk, col::text,
                       hotSelClip == cl.clipId && hotSelBtn == 1, fSmall);
                button(h, cl.savesel, L"Save sel", col::accentDk, col::text,
                       hotSelClip == cl.clipId && hotSelBtn == 2, fSmall);
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
        // insertion caret while reordering a clip within the library
        if (mode == Mode::CardDrag && dragged && !cards.empty()) {
            POINT cp; GetCursorPos(&cp); ScreenToClient(hwnd, &cp);
            if (PtInRect(&rcLibrary, cp)) {
                RECT bar = libDropCaret(cp, libInsertIndex(cp));
                fill(h, bar, col::accent);
            }
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
        // playhead handle on the ruler (makes the vertical line read as a playhead)
        if (timelinePlaying || (!engine.isPlaying() && previewClipId < 0)) {
            int px = frameToX(playheadFrame);
            int laneLeft = rcTimeline.left + trackHeaderW;
            if (px >= laneLeft && px <= rcTimeline.right) {
                POINT tri[3] = { { px - S(5), rr.top }, { px + S(5), rr.top }, { px, rr.bottom } };
                HBRUSH b = CreateSolidBrush(col::playhead); HPEN pen = CreatePen(PS_SOLID, 1, col::playhead);
                HGDIOBJ ob = SelectObject(h, b), op = SelectObject(h, pen);
                Polygon(h, tri, 3);
                SelectObject(h, ob); SelectObject(h, op); DeleteObject(b); DeleteObject(pen);
            }
        }

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
            // While actively dragging, the moved clip is drawn as a floating ghost
            // (below); leave just a faint outline in its home slot.
            if (moving && dragged) {
                roundFill(h, pl.rc, col::panel, col::cardEdge, S(6));
                continue;
            }
            roundFill(h, pl.rc, moving ? col::clipBlkSel : col::clipBlk, col::cardEdge, S(6));
            RECT wv = { pl.rc.left + S(3), pl.rc.top + S(18), pl.rc.right - S(3), pl.rc.bottom - S(4) };
            if (c && c->buffer && c->peaks && wv.right > wv.left) {
                SaveDC(h); IntersectClipRect(h, wv.left, wv.top, wv.right, wv.bottom);
                wf::draw(h, wv, *c->buffer, *c->peaks, 0, c->frames(), RGB(180, 210, 245));
                // The selection belongs to the clip, so it shows here too -- this
                // block is the same audio as the library card above.
                if (selectionCovers(pl.clipId)) drawSelOverlay(h, wv, *c);
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
        // drag ghost when sliding a placed clip around (shows where it will land)
        if (mode == Mode::ClipMove && dragged) {
            POINT p; GetCursorPos(&p); ScreenToClient(hwnd, &p);
            int tid = moveTrackId; trackAtPoint(p, tid);
            const Clip* c = doc.project().findClip(dragClipId);
            if (c) {
                int ignore = (tid == moveTrackId) ? moveIndex : -1;
                int64_t start = snapFrame(tid, xToFrame(p.x) - moveGrabOffset, c->frames(), ignore);
                for (auto& tl : trackLays) if (tl.trackId == tid) {
                    int x0 = frameToX(start), x1 = frameToX(start + c->frames());
                    RECT g = { x0, tl.lane.top + S(4), x1, tl.lane.bottom - S(4) };
                    const Track* tk = doc.project().findTrack(tid);
                    bool ok = tk && !tk->overlaps(start, c->frames(), ignore);
                    roundFill(h, g, ok ? col::clipBlkSel : col::stop, col::text, S(6));
                    // waveform + name so the ghost reads as the actual clip
                    RECT wv = { g.left + S(3), g.top + S(18), g.right - S(3), g.bottom - S(4) };
                    if (c->buffer && c->peaks && wv.right > wv.left) {
                        SaveDC(h); IntersectClipRect(h, wv.left, wv.top, wv.right, wv.bottom);
                        wf::draw(h, wv, *c->buffer, *c->peaks, 0, c->frames(), RGB(180, 210, 245));
                        RestoreDC(h, -1);
                    }
                    RECT nm = { g.left + S(6), g.top + S(2), g.right - S(4), g.top + S(18) };
                    textOut(h, nm, c->name, col::text, fSmall, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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
        // vertical scrollbar when there are more tracks than fit
        {
            RECT gutter, thumb;
            if (tlVScrollGeom(gutter, thumb)) {
                fill(h, gutter, col::panel);
                roundFill(h, thumb, mode == Mode::TlVScroll ? col::accentDk : col::btn, col::cardEdge, S(4));
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
    // The laid-out card frames, in library order — the input to the drop geometry
    // in layout.h. Only built while a drag is live, so the copy is not hot.
    std::vector<RECT> cardRects() const {
        std::vector<RECT> r; r.reserve(cards.size());
        for (auto& c : cards) r.push_back(c.card);
        return r;
    }
    // Reading-order insertion index for a library drag/drop at p (0..cards.size()).
    int libInsertIndex(POINT p) const { return layout::insertIndex(cardRects(), p); }
    // The insertion caret to paint for that drop. See layout::caretAnchor for why the
    // caret can sit *after* a card rather than always before the one it inserts at.
    RECT libDropCaret(POINT p, int idx) const {
        const std::vector<RECT> rects = cardRects();
        const layout::CaretAnchor a = layout::caretAnchor(rects, p, idx);
        if (a.card < 0) return RECT{};
        const RECT& r = rects[a.card];
        const int cx = a.trailing ? r.right + S(6) : r.left - S(6);
        return RECT{ cx - S(1), r.top, cx + S(2), r.bottom };
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

    // Play / pause a clip preview. `useSel` asks for the selection-only audition;
    // it is ignored when the clip has no selection. `ownRangeOnly` marks a control
    // that speaks for one range only (the editor's button pair) as opposed to one
    // that stands for whatever is playing (the card, the Space key).
    //
    // The decision itself lives in transport::decide so it can be tested headlessly;
    // this is just the translation to and from the engine. Play/resume always
    // starts at previewCursor, so clicking the waveform while paused or stopped
    // moves where playback picks up.
    void togglePreview(int clipId, bool useSel, bool ownRangeOnly = false) {
        transport::State st;
        st.clipId = previewClipId; st.isSel = previewIsSel;
        st.begin = previewBegin;   st.end = previewEnd;
        st.playing = engine.isPlaying(); st.paused = engine.isPaused();
        st.seekPending = previewSeekPending; st.timeline = timelinePlaying;

        transport::Press pr;
        pr.clipId = clipId;
        pr.wantSel = useSel && hasSel() && selClipId == clipId;
        pr.ownRangeOnly = ownRangeOnly;
        pr.selBegin = selStart; pr.selEnd = selEnd;

        transport::Plan plan = transport::decide(st, pr);
        switch (plan.act) {
        case transport::Act::Pause:  engine.pause();  refresh(); return;
        case transport::Act::Resume: engine.resume(); refresh(); return;
        case transport::Act::Restart: break;
        }
        const int64_t from = plan.from == transport::From::Cursor   ? previewCursor
                           : plan.from == transport::From::SelStart ? selStart
                                                                    : 0;
        startPreview(clipId, from, pr.wantSel);
    }
    // The library card's play button auditions the selection when there is one.
    void togglePlayClip(int clipId) { togglePreview(clipId, true); }

    // (Re)arm a preview source for `clipId` and start it at `startFrame`.
    void startPreview(int clipId, int64_t startFrame, bool useSel) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer) return;
        const bool sel = useSel && hasSel() && selClipId == clipId;
        const int64_t begin = sel ? selStart : 0;
        const int64_t end   = sel ? selEnd   : c->frames();
        if (end <= begin) return;
        if (startFrame < begin || startFrame >= end) startFrame = begin;   // restart if at the end
        timelinePlaying = false;
        previewClipId = clipId; previewIsSel = sel;
        previewBegin = begin; previewEnd = end;
        previewCursor = startFrame; previewSeekPending = false;
        engine.play(std::make_shared<BufferSource>(c->buffer, begin, end, c->gain), startFrame - begin);
        refresh();
    }
    void playSelection() { if (hasSel()) startPreview(selClipId, selStart, true); }

    // Move the play cursor. Takes effect immediately while playing; otherwise it is
    // remembered and honoured by the next play/resume.
    void seekClip(int clipId, int64_t frame) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c) return;
        frame = std::max<int64_t>(0, std::min(frame, c->frames()));
        // Stay inside a selection audition only while the target is still in range.
        const bool keepSel = previewIsSel && previewClipId == clipId && hasSel() &&
                             selClipId == clipId && frame >= selStart && frame < selEnd;
        const bool playing = !timelinePlaying && previewClipId == clipId && engine.isPlaying();
        timelinePlaying = false;
        if (playing) { startPreview(clipId, frame, keepSel); return; }
        previewClipId = clipId; previewIsSel = keepSel;
        previewCursor = frame; previewSeekPending = true;
        refresh();
    }

    // True when the waveform selection belongs to `clipId` and spans something —
    // i.e. a "(selection)" menu item may run on that clip. A selection left over
    // on some *other* clip must never silently narrow an operation.
    bool selectionCovers(int clipId) const {
        return hasSel() && selClipId == clipId && selEnd > selStart;
    }

    // "the selection in 'take 2' (0:12.30 – 0:18.00, 5.70 s)" — the "Applies to:"
    // line of both effect dialogs, so the range chosen in the menu is restated
    // where the settings are chosen.
    std::wstring selectionScopeLabel(int clipId) const {
        const Clip* c = doc.project().findClip(clipId);
        const std::wstring name = c ? c->name : L"clip";
        const double rate = (c && c->buffer && c->buffer->sampleRate > 0) ? c->buffer->sampleRate : 48000.0;
        const double t0 = selStart / rate, t1 = selEnd / rate;
        wchar_t d[160];
        swprintf(d, 160, L" (%s \u2013 %s, %.2f s)", fmtTime(t0).c_str(), fmtTime(t1).c_str(), t1 - t0);
        return L"the selection in '" + name + L"'" + d;
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
        int id = c->id; std::wstring nm = c->name;
        auto slice = sliceBuffer(*c->buffer, selStart, selEnd);

        // Crop only trims the clip in-editor (undoable). We never overwrite or
        // delete the original file on disk — to keep a cropped copy, use
        // "Save selection as clip" or export.
        doc.replaceClipBuffer(id, slice, L"Crop '" + nm + L"'");
        selStart = 0; selEnd = 0; selClipId = -1;
        refresh();
    }

    // "Crop to selection" keeps the selection and throws away the rest; these
    // two are its opposite, and the manual counterpart to "Remove non-voice".
    // The automatic detector can only remove what it can recognise, and some
    // non-speech events (a creak, a swallow, a shuffle that rings) are harmonic
    // and syllable-length -- indistinguishable from voice at any sensitivity.
    // When the user can hear it but the detector can't, they select it and say
    // so directly.
    //
    // `keepLength` silences the range in place, which is what you want between
    // sentences: the pause stays the length it was, so nothing downstream in the
    // timeline shifts. Otherwise the range is cut out and the gap closed.
    void removeSelectedRange(int clipId, bool keepLength) {
        if (!selectionCovers(clipId)) return;
        Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer) return;
        const std::wstring nm = c->name;
        auto edited = keepLength ? dsp::silenceRange(*c->buffer, selStart, selEnd)
                                 : dsp::deleteRange(*c->buffer, selStart, selEnd);
        if (!edited) {
            MessageBoxW(hwnd, L"The selection is empty.", L"Edit selection", MB_ICONINFORMATION);
            return;
        }
        doc.replaceClipBuffer(clipId, edited,
                              (keepLength ? L"Silence selection in '" : L"Delete selection from '") + nm + L"'");
        // Silencing leaves the timeline intact, so the selection still points at
        // the same audio and is worth keeping (the user can audition the result
        // with "Play selection"). Deleting moves everything after the cut, so
        // the old range no longer means anything.
        if (!keepLength) { selStart = selEnd = 0; selClipId = -1; }
        refresh();
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

    // Run the voice cleaner over one clip.
    void voiceCleanClip(int clipId) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c) return;
        voiceCleanClips({ clipId }, L"'" + c->name + L"'", false);
    }
    // ...or over just the waveform selection inside it.
    void voiceCleanClipSelection(int clipId) {
        if (!selectionCovers(clipId)) return;
        voiceCleanClips({ clipId }, selectionScopeLabel(clipId), true);
    }
    // Voice-clean every clip in the library (whole project).
    void voiceCleanAllClips() {
        std::vector<int> ids;
        for (auto& c : doc.project().library) ids.push_back(c.id);
        voiceCleanClips(ids, L"all clips", false);
    }
    // Voice-clean the (library) clips referenced by everything placed on a track.
    void voiceCleanTrack(int trackId) {
        const Track* t = doc.project().findTrack(trackId);
        if (!t) return;
        std::vector<int> ids;
        for (auto& pc : t->clips) ids.push_back(pc.clipId);
        voiceCleanClips(ids, L"track '" + t->name + L"'", false);
    }
    // Show the cleaner options once, then denoise the given clips as a single
    // undo step. Duplicate / empty clips are skipped. `selectionOnly` (only ever
    // set for a single-clip scope that the selection covers) keeps just the
    // selected range of the result.
    void voiceCleanClips(const std::vector<int>& ids, const std::wstring& scopeLabel,
                         bool selectionOnly) {
        std::vector<int> targets;
        for (int id : ids) {
            bool dup = false;
            for (int t : targets) if (t == id) { dup = true; break; }
            if (dup) continue;
            const Clip* c = doc.project().findClip(id);
            if (c && c->buffer && c->buffer->frames() > 0) targets.push_back(id);
        }
        if (targets.empty()) {
            MessageBoxW(hwnd, L"No audio to clean here.", L"Voice cleaner", MB_ICONINFORMATION);
            return;
        }
        // The current waveform selection (on any clip) doubles as the noise
        // sample for the Audacity-style profile algorithm's Get Noise Profile.
        AudioBufferPtr noiseSel;
        const bool selOnly = selectionOnly && targets.size() == 1 && selectionCovers(targets[0]);
        std::wstring scope = scopeLabel;
        if (targets.size() > 1) scope += L" (" + std::to_wstring(targets.size()) + L" clips)";
        dlg::VoiceCleanerContext ctx;
        ctx.opts = &nrOpts;
        ctx.profile = &noiseProfile;
        ctx.profileDesc = &noiseProfileDesc;
        ctx.scopeLabel = scope;
        if (hasSel()) {
            const Clip* sc = doc.project().findClip(selClipId);
            if (sc && sc->buffer) {
                noiseSel = sliceBuffer(*sc->buffer, selStart, selEnd);
                ctx.noiseSelection = noiseSel.get();
                ctx.selectionDesc = L"'" + sc->name + L"'";
            }
        }
        bool okDlg = dlg::voiceCleaner(hwnd, ctx);
        if (ctx.captured) rememberCapture(noiseProfile, noiseProfileDesc);
        if (!okDlg) return;
        dsp::NROptions opts = nrOpts;             // nrOpts persists for the session
        opts.noiseProfile = &noiseProfile;        // bind profile only for this call
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        std::vector<std::pair<int, AudioBufferPtr>> updates;
        int failed = 0;
        for (int id : targets) {
            const Clip* c = doc.project().findClip(id);
            if (!c || !c->buffer) continue;
            auto cleaned = dsp::denoise(*c->buffer, opts);
            // "Only the selection": the whole clip is analysed and cleaned (so the
            // noise estimate still sees the quiet gaps), then only the selected
            // range of that result is kept.
            if (cleaned && selOnly)
                cleaned = dsp::blendProcessedRange(*c->buffer, *cleaned, selStart, selEnd);
            if (cleaned) updates.push_back({ id, cleaned });
            else ++failed;
        }
        SetCursor(old);
        if (updates.empty()) {
            MessageBoxW(hwnd, L"Voice cleaner failed.", L"Voice cleaner", MB_ICONWARNING);
            return;
        }
        stopAll();   // buffers are changing under any active playback
        // scopeLabel already says whether this was a clip or a selection within one.
        std::wstring desc = targets.size() == 1
            ? L"Voice cleaner on " + scopeLabel
            : L"Voice cleaner (" + scopeLabel + L", " + std::to_wstring(updates.size()) + L" clips)";
        doc.replaceClipBuffers(updates, desc);
        refresh();
        if (failed)
            MessageBoxW(hwnd, L"Some clips could not be cleaned.", L"Voice cleaner", MB_ICONWARNING);
    }

    // ---- Remove non-voice (voice isolation) ----
    void removeNonVoiceClip(int clipId) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c) return;
        removeNonVoiceClips({ clipId }, L"'" + c->name + L"'", false);
    }
    // ...or over just the waveform selection inside it.
    void removeNonVoiceClipSelection(int clipId) {
        if (!selectionCovers(clipId)) return;
        removeNonVoiceClips({ clipId }, selectionScopeLabel(clipId), true);
    }
    void removeNonVoiceAllClips() {
        std::vector<int> ids;
        for (auto& c : doc.project().library) ids.push_back(c.id);
        removeNonVoiceClips(ids, L"all clips", false);
    }
    void removeNonVoiceTrack(int trackId) {
        const Track* t = doc.project().findTrack(trackId);
        if (!t) return;
        std::vector<int> ids;
        for (auto& pc : t->clips) ids.push_back(pc.clipId);
        removeNonVoiceClips(ids, L"track '" + t->name + L"'", false);
    }
    // Show the isolation options once, then process the given clips as a single
    // undo step. Duplicate / empty clips are skipped. `selectionOnly` (only ever
    // set for a single-clip scope that the selection covers) keeps just the
    // selected range of the result.
    void removeNonVoiceClips(const std::vector<int>& ids, const std::wstring& scopeLabel,
                             bool selectionOnly) {
        std::vector<int> targets;
        for (int id : ids) {
            bool dup = false;
            for (int t : targets) if (t == id) { dup = true; break; }
            if (dup) continue;
            const Clip* c = doc.project().findClip(id);
            if (c && c->buffer && c->buffer->frames() > 0) targets.push_back(id);
        }
        if (targets.empty()) {
            MessageBoxW(hwnd, L"No audio to process here.", L"Remove non-voice", MB_ICONINFORMATION);
            return;
        }
        const bool selOnly = selectionOnly && targets.size() == 1 && selectionCovers(targets[0]);
        std::wstring scope = scopeLabel;
        if (targets.size() > 1) scope += L" (" + std::to_wstring(targets.size()) + L" clips)";
        dlg::VoiceIsolateContext ctx;
        ctx.opts = &viOpts;
        ctx.scopeLabel = scope;
        if (!dlg::voiceIsolate(hwnd, ctx)) return;

        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        std::vector<std::pair<int, AudioBufferPtr>> updates;
        double removedSec = 0.0; int segments = 0;
        for (int id : targets) {
            const Clip* c = doc.project().findClip(id);
            if (!c || !c->buffer) continue;
            dsp::VoiceIsolateStats st;
            // "Only the selection": voice detection still runs over the whole clip
            // (it needs the surrounding context), but only the selected range of
            // the result is kept — and only that range is counted in the stats.
            auto processed = selOnly
                ? dsp::isolateVoice(*c->buffer, viOpts, &st, selStart, selEnd)
                : dsp::isolateVoice(*c->buffer, viOpts, &st);
            if (processed && selOnly)
                processed = dsp::blendProcessedRange(*c->buffer, *processed, selStart, selEnd);
            if (!processed) continue;
            removedSec += st.removedSeconds;
            segments += st.segments;
            updates.push_back({ id, processed });
        }
        SetCursor(old);
        if (updates.empty()) {
            MessageBoxW(hwnd, L"Nothing could be processed.", L"Remove non-voice", MB_ICONWARNING);
            return;
        }
        stopAll();   // buffers are changing under any active playback
        // scopeLabel already says whether this was a clip or a selection within one.
        std::wstring desc = (viOpts.residue ? L"Preview removed non-voice in "
                                            : L"Remove non-voice from ") + scopeLabel;
        doc.replaceClipBuffers(updates, desc);
        refresh();
        // The counts cover only what was actually changed, so say which that was.
        const wchar_t* where = selOnly ? L" (within the selection)" : L"";
        wchar_t msg[320];
        swprintf(msg, 320,
                 viOpts.residue
                     ? L"Kept only the non-voice material: %.1f s across %d voice segment(s)%s.\n\n"
                       L"Undo (Ctrl+Z) to go back."
                     : L"Silenced %.1f s of non-voice, keeping %d voice segment(s)%s.\n\n"
                       L"Undo (Ctrl+Z) to go back.",
                 removedSec, segments, where);
        MessageBoxW(hwnd, msg, L"Remove non-voice", MB_ICONINFORMATION);
    }

    // Capture the Audacity-style noise profile from the current selection
    // (right-click shortcut; the voice cleaner dialog has the same button).
    void captureNoiseProfileFromSelection() {
        if (!hasSel()) {
            MessageBoxW(hwnd,
                L"Select a noise-only region first.\n\n"
                L"Drag across a clip's waveform over a stretch that contains only "
                L"background noise (no speech), then choose \u201CCapture noise from "
                L"selection\u201D again. The capture is then available under "
                L"\u201CApply noise capture\u201D for any clip.",
                L"Capture noise", MB_ICONINFORMATION);
            return;
        }
        const Clip* c = doc.project().findClip(selClipId);
        if (!c || !c->buffer) return;
        auto slice = sliceBuffer(*c->buffer, selStart, selEnd);
        dsp::NoiseProfile p = dsp::computeNoiseProfile(*slice);
        if (!p.valid()) {
            MessageBoxW(hwnd,
                L"Selected noise profile is too short.\n"
                L"Select at least ~50 ms of noise-only audio.",
                L"Voice cleaner", MB_ICONINFORMATION);
            return;
        }
        noiseProfile = std::move(p);
        wchar_t d[160];
        swprintf(d, 160, L"%.2f s from '%s'", noiseProfile.seconds, c->name.c_str());
        noiseProfileDesc = d;
        rememberCapture(noiseProfile, noiseProfileDesc);
        MessageBoxW(hwnd,
            (L"Noise capture saved: " + noiseProfileDesc +
             L"\n\nApply it from any clip's right-click menu \u2192 Voice cleaner "
             L"\u2192 Apply noise capture, or via the voice cleaner dialog.").c_str(),
            L"Voice cleaner", MB_ICONINFORMATION);
    }

    // Capture a noise profile from an entire clip buffer (no selection needed).
    // Useful when the clip is nothing but noise, or as a quick whole-clip
    // estimate. Feeds the same recents list as the selection capture.
    void captureNoiseProfileFromClip(int clipId) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer || c->buffer->frames() == 0) return;
        dsp::NoiseProfile p = dsp::computeNoiseProfile(*c->buffer);
        if (!p.valid()) {
            MessageBoxW(hwnd,
                L"This clip is too short to build a noise profile.\n"
                L"It needs at least ~50 ms of audio.",
                L"Voice cleaner", MB_ICONINFORMATION);
            return;
        }
        noiseProfile = std::move(p);
        wchar_t d[160];
        swprintf(d, 160, L"whole clip '%s' (%.2f s)", c->name.c_str(), noiseProfile.seconds);
        noiseProfileDesc = d;
        rememberCapture(noiseProfile, noiseProfileDesc);
        MessageBoxW(hwnd,
            (L"Noise capture saved: " + noiseProfileDesc +
             L"\n\nApply it from any clip's right-click menu \u2192 Voice cleaner "
             L"\u2192 Apply noise capture, or via the voice cleaner dialog.").c_str(),
            L"Voice cleaner", MB_ICONINFORMATION);
    }

    // Capture from the full-window editor: use the active selection if one is
    // present, else fall back to the whole clip.
    void captureNoiseProfileFromEditor() {
        if (editClipId < 0) return;
        if (hasSel() && selClipId == editClipId)
            captureNoiseProfileFromSelection();
        else
            captureNoiseProfileFromClip(editClipId);
    }

    // Add the active capture to the front of the recent-captures list (most
    // recently used first, capped, no duplicate of the current front). The
    // profile itself is the cached per-capture processing result.
    void rememberCapture(const dsp::NoiseProfile& prof, const std::wstring& desc) {
        if (!prof.valid()) return;
        // Skip if this is already the most-recent capture (e.g. applying it again
        // through the dialog without recapturing).
        if (!noiseCaptures.empty() && noiseCaptures.front().desc == desc &&
            noiseCaptures.front().profile.windows == prof.windows &&
            noiseCaptures.front().profile.seconds == prof.seconds)
            return;
        noiseCaptures.insert(noiseCaptures.begin(), NoiseCapture{ prof, desc });
        const size_t kMaxCaptures = 8;
        if (noiseCaptures.size() > kMaxCaptures) noiseCaptures.resize(kMaxCaptures);
    }

    // Apply a remembered noise capture to one clip using the current profile
    // options (one undo step). Reuses the capture's cached NoiseProfile; moves
    // it to the front of the recents as most-recently-used.
    void applyCaptureToClip(int clipId, int captureIdx) {
        if (captureIdx < 0 || captureIdx >= (int)noiseCaptures.size()) return;
        const Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer || c->buffer->frames() == 0) return;
        // Copy into the stable active-profile member (never hold a pointer into
        // the vector across the reorder below).
        noiseProfile = noiseCaptures[captureIdx].profile;
        noiseProfileDesc = noiseCaptures[captureIdx].desc;
        std::wstring capDesc = noiseProfileDesc;
        if (noiseProfile.sampleRate != c->buffer->sampleRate) {
            MessageBoxW(hwnd,
                L"This noise capture was made at a different sample rate than the clip, "
                L"so it can't be applied. Capture a new profile from this project.",
                L"Voice cleaner", MB_ICONINFORMATION);
            return;
        }
        nrOpts.algorithm = dsp::NRAlgorithm::Profile;
        dsp::NROptions opts = nrOpts;
        opts.noiseProfile = &noiseProfile;
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        auto cleaned = dsp::denoise(*c->buffer, opts);
        SetCursor(old);
        if (!cleaned) {
            MessageBoxW(hwnd, L"Voice cleaner failed for this clip.", L"Voice cleaner", MB_ICONWARNING);
            return;
        }
        stopAll();
        doc.replaceClipBuffers({ { clipId, cleaned } },
            L"Voice cleaner on '" + c->name + L"' (" + capDesc + L")");
        // Promote the used capture to most-recently-used.
        if (captureIdx != 0) {
            NoiseCapture used = noiseCaptures[captureIdx];
            noiseCaptures.erase(noiseCaptures.begin() + captureIdx);
            noiseCaptures.insert(noiseCaptures.begin(), std::move(used));
        }
        refresh();
    }

    // --------------------------------------------------------- project I/O
    void setTitle() {
        std::wstring t = L"Audio Clip Editor";
        if (!projectPath.empty()) t += std::wstring(L" \u2014 ") + PathFindFileNameW(projectPath.c_str());
        if (projectModified()) t += L" *";
        SetWindowTextW(hwnd, t.c_str());
    }
    void openProjectFile() {
        if (!confirmDiscardChanges()) return;
        std::wstring path = dlg::openProject(hwnd);
        if (path.empty()) return;
        if (!doc.loadProject(path)) { MessageBoxW(hwnd, L"Could not open project.", L"Open", MB_ICONWARNING); return; }
        stopAll();
        // Follow the loaded project's internal rate so playback resamples correctly.
        rate = doc.project().sampleRate > 0 ? doc.project().sampleRate : rate;
        engine.setSourceRate(rate);
        projectPath = path;
        previewClipId = -1; timelinePlaying = false;
        libScroll = tlScrollX = tlScrollY = 0;
        restoreViewState();   // selection + playhead saved with the project
        setTitle(); clampScroll(); refresh();
    }
    // The waveform selection and playhead ride along in the .acep file; they are
    // UI state, so they live in App and are pushed into Document whenever the
    // saved state matters (never through the undo tree). Always ask through
    // projectModified() rather than doc.isModified() directly, so the live
    // selection is in Document before the comparison happens.
    bool projectModified() { storeViewState(); return doc.isModified(); }
    void storeViewState() {
        ViewState& v = doc.view();
        v.selClipId = hasSel() ? selClipId : -1;
        v.selStart = hasSel() ? selStart : 0;
        v.selEnd = hasSel() ? selEnd : 0;
        v.playheadFrame = playheadFrame;
    }
    void restoreViewState() {
        const ViewState& v = doc.view();   // loadProject already validated it
        selClipId = v.selClipId;
        selStart = v.selStart;
        selEnd = v.selEnd;
        playheadFrame = v.playheadFrame;
    }
    bool saveProjectAs() {
        std::wstring suggested = projectPath.empty() ? L"Untitled" : PathFindFileNameW(projectPath.c_str());
        std::wstring path = dlg::saveProject(hwnd, suggested);
        if (path.empty()) return false;
        storeViewState();
        if (!doc.saveProject(path)) { MessageBoxW(hwnd, L"Could not save project.", L"Save", MB_ICONWARNING); return false; }
        projectPath = path; setTitle(); return true;
    }
    bool saveProjectFile() {
        if (projectPath.empty()) return saveProjectAs();
        storeViewState();
        if (!doc.saveProject(projectPath)) {
            MessageBoxW(hwnd, L"Could not save project.", L"Save", MB_ICONWARNING);
            return false;
        }
        setTitle();
        return true;
    }
    // Guard against losing unsaved work before an action that discards the current
    // project (exit, open another project). Returns true if the caller may proceed
    // (saved, or the user chose to discard); false to cancel the action.
    bool confirmDiscardChanges() {
        if (!projectModified()) return true;
        std::wstring name = projectPath.empty() ? L"this project"
                          : std::wstring(L"\u201C") + PathFindFileNameW(projectPath.c_str()) + L"\u201D";
        int r = MessageBoxW(hwnd,
            (L"Save changes to " + name + L" before closing?").c_str(),
            L"Audio Clip Editor",
            MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return false;
        if (r == IDNO) return true;      // discard
        return saveProjectFile();         // IDYES: proceed only if the save succeeds
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

    // Write one library clip -- or just the waveform selection inside it -- out as
    // a new audio file. This is an *export*, not a save-over: the clip, the
    // project and the original source file are all left untouched, since edits
    // live in the .acep and the source on disk is never written to.
    void exportClipAudio(int clipId, bool selectionOnly) {
        const Clip* c = doc.project().findClip(clipId);
        if (!c || !c->buffer || c->buffer->frames() == 0) {
            MessageBoxW(hwnd, L"This clip has no audio to export.", L"Export clip", MB_ICONINFORMATION);
            return;
        }
        if (selectionOnly && !selectionCovers(clipId)) return;
        AudioBufferPtr buf = selectionOnly ? sliceBuffer(*c->buffer, selStart, selEnd) : c->buffer;
        if (!buf || buf->frames() == 0) {
            MessageBoxW(hwnd, L"The selection is empty.", L"Export selection", MB_ICONINFORMATION);
            return;
        }
        // Default to the clip's own rate and channel count so a plain "export"
        // doesn't quietly resample or upmix a mono take; the dialog can override.
        mfio::ExportOptions o;
        o.sampleRate = buf->sampleRate > 0 ? buf->sampleRate : rate;
        o.channels = buf->channels >= 2 ? 2 : 1;
        o.format = mfio::ExportFormat::WAV;
        o.bitsPerSample = 24;
        o.bitrateKbps = 192;
        std::wstring outPath;
        const std::wstring suggested =
            mfio::safeFileName(c->name) + (selectionOnly ? L" (selection)" : L"");
        if (!dlg::exportOptions(hwnd, o, outPath, suggested)) return;
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        std::wstring err; bool ok = mfio::encodeFile(outPath, *buf, o, &err);
        SetCursor(old);
        if (!ok) MessageBoxW(hwnd, err.c_str(), L"Could not export", MB_ICONWARNING);
    }

    void showControls() {
        MessageBoxW(hwnd,
            L"Library clips:\n"
            L"  \u2022 Click the \u25B6 button to play/pause a clip\n"
            L"  \u2022 Click-drag across the waveform to select a section\n"
            L"  \u2022 Double-click a clip (or right-click \u2192 Open in editor) for a full-window editor\n"
            L"  \u2022 Drag a clip up onto a track to place it (drops at any offset)\n"
            L"  \u2022 Right-click for save-selection, crop, normalize, voice cleaner\u2026\n"
            L"  \u2022 Voice cleaner cleans one clip, or all clips in the project\n"
            L"  \u2022 Remove non-voice silences bumps, shuffling and room tone\n"
            L"  \u2022 Drag the volume slider to change a clip's level\n\n"
            L"Full-window editor:\n"
            L"  \u2022 Drag across the big waveform to select; buttons to play/crop/save\n"
            L"  \u2022 Fine-tune edges: two zoomed strips for the selection's start & end\n"
            L"  \u2022 Drag the strip knobs (or wheel-zoom a strip) to nudge exact points\n"
            L"  \u2022 Esc closes the editor\n\n"
            L"Timeline:\n"
            L"  \u2022 Drag placed clips to move them (snaps to neighbours)\n"
            L"  \u2022 Drag a track's volume slider to change the track level\n"
            L"  \u2022 Right-click a track header to clean/isolate voice, rename or remove the track\n"
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
        engine.stop(); timelinePlaying = false;
        previewClipId = -1; previewIsSel = false; previewSeekPending = false;
        previewBegin = previewEnd = 0; previewCursor = 0;
        refresh();
    }

    // --------------------------------------------------------- undo / redo
    void doUndo() { if (doc.canUndo()) { doc.undo(); afterHistory(); } }
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
    // Called after an edit that may have replaced or removed clips: undo / redo, or
    // deleting a clip, a track, or a clip's placement on a track.
    void afterHistory() {
        stopAll();            // playback may reference buffers this edit replaced
        validateSelection();
        clampScroll(); refresh();
    }

    // Keep the waveform selection across such an edit unless it can no longer be
    // honoured. Most of these edits don't touch the selected clip's audio at all --
    // removing a clip's placement from a track leaves the library clip untouched --
    // and a selection is something the user positioned by hand, so throwing it away
    // as a blanket precaution loses their work for no reason. `clampSelection` is
    // the same rule a loaded project's selection goes through.
    void validateSelection() { clampSelection(doc.project(), selClipId, selStart, selEnd); }

    // --------------------------------------------------------- scroll
    // Vertical scrollbar for the tracks pane. Returns false when all tracks fit
    // (nothing to scroll). `gutter` is the full track, `thumb` the draggable knob.
    bool tlVScrollGeom(RECT& gutter, RECT& thumb) const {
        int tlVis = rcTimeline.bottom - (rcTimeline.top + rulerH);
        if (tlVis <= 0 || tlContentH <= tlVis) return false;
        int gw = S(12);
        gutter = { rcTimeline.right - gw, rcTimeline.top + rulerH, rcTimeline.right, rcTimeline.bottom };
        int trackH = gutter.bottom - gutter.top;
        int thumbH = std::max(S(28), (int)((double)tlVis / tlContentH * trackH));
        thumbH = std::min(thumbH, trackH);
        int maxY = std::max(1, tlContentH - tlVis);
        int travel = std::max(0, trackH - thumbH);
        int thumbTop = gutter.top + (int)((double)tlScrollY / maxY * travel);
        thumb = { gutter.left + S(2), thumbTop, gutter.right - S(1), thumbTop + thumbH };
        return true;
    }
    // Map a thumb-top pixel position back to a tlScrollY value.
    void setVScrollFromThumbTop(int thumbTop) {
        RECT gutter, thumb;
        if (!tlVScrollGeom(gutter, thumb)) return;
        int trackH = gutter.bottom - gutter.top;
        int thumbH = thumb.bottom - thumb.top;
        int travel = std::max(1, trackH - thumbH);
        int tlVis = rcTimeline.bottom - (rcTimeline.top + rulerH);
        int maxY = std::max(0, tlContentH - tlVis);
        double frac = (double)(thumbTop - gutter.top) / travel;
        tlScrollY = (int)(std::max(0.0, std::min(1.0, frac)) * maxY);
        clampScroll();
    }

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
        selSaveClipId = selClipId; selSaveStart = selStart; selSaveEnd = selEnd;
        if (editorActive()) { edDown(p, dbl); return; }
        SetFocus(hwnd);
        downPt = p; dragged = false;
        int tb = tbAt(p);
        if (PtInRect(&rcTransport, p)) {
            RECT zh = sliderHit(zoomRc);
            if (PtInRect(&zh, p)) {
                mode = Mode::ZoomDrag;
                setZoomFrac((float)(p.x - zoomRc.left) / std::max(1, (int)(zoomRc.right - zoomRc.left)));
                SetCapture(hwnd); clampScroll(); refresh(); return;
            }
            if (tb >= 0) handleTB(tb);
            return;
        }

        // library
        if (PtInRect(&rcLibrary, p)) {
            const CardLayout* cl = cardAt(p);
            if (!cl) { return; }
            // selection-action buttons (overlay the top of the waveform)
            if (hasSel() && selClipId == cl->clipId) {
                if (PtInRect(&cl->crop, p))    { cropSelection(); return; }
                if (PtInRect(&cl->savesel, p)) { saveSelectionAsClip(); return; }
            }
            if (PtInRect(&cl->play, p)) { togglePlayClip(cl->clipId); return; }
            if (PtInRect(&cl->del, p)) { deleteClipConfirm(cl->clipId); return; }
            { RECT vh = sliderHit(cl->vol);
              if (PtInRect(&vh, p)) {
                mode = Mode::ClipVolume; dragClipId = cl->clipId;
                if (Clip* c = doc.project().findClip(cl->clipId)) c->gain = sliderGainAt(cl->vol, p.x);
                SetCapture(hwnd); refresh(); return;
              } }
            if (PtInRect(&cl->wave, p)) {
                if (dbl) { openClipEditor(cl->clipId); return; }
                // begin selection / seek
                mode = Mode::WaveSelect; dragClipId = cl->clipId;
                const Clip* c = doc.project().findClip(cl->clipId);
                int64_t nf = c ? c->frames() : 0;
                int64_t f = waveFrameAt(*cl, nf, p.x);
                // If a selection already exists on this clip, grab its nearer edge
                // so one side can be adjusted without redrawing the whole selection.
                waveEdgeDrag = false;
                if (hasSel() && selClipId == cl->clipId && nf > 0) {
                    int ww = std::max(1, (int)(cl->wave.right - cl->wave.left));
                    int sx0 = cl->wave.left + (int)((double)selStart / nf * ww);
                    int sx1 = cl->wave.left + (int)((double)selEnd / nf * ww);
                    int edge = hitSelEdge(p.x, sx0, sx1);
                    if (edge == 1)      { waveAnchor = selEnd;   waveEdgeDrag = true; }
                    else if (edge == 2) { waveAnchor = selStart; waveEdgeDrag = true; }
                }
                if (!waveEdgeDrag) { selClipId = cl->clipId; selStart = selEnd = f; waveAnchor = f; }
                SetCapture(hwnd); refresh(); return;
            }
            if (PtInRect(&cl->top, p)) {
                if (dbl) { openClipEditor(cl->clipId); return; }
                // start drag-to-timeline
                mode = Mode::CardDrag; dragClipId = cl->clipId; moveGrabOffset = 0;
                SetCapture(hwnd); return;
            }
            return;
        }

        // timeline
        if (PtInRect(&rcTimeline, p)) {
            // vertical scrollbar (takes priority over lane/header hits at the far right)
            {
                RECT gutter, thumb;
                if (tlVScrollGeom(gutter, thumb) && PtInRect(&gutter, p)) {
                    mode = Mode::TlVScroll;
                    if (PtInRect(&thumb, p)) vscrollGrab = p.y - thumb.top;
                    else { vscrollGrab = (thumb.bottom - thumb.top) / 2; setVScrollFromThumbTop(p.y - vscrollGrab); }
                    SetCapture(hwnd); refresh(); return;
                }
            }
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
        if (editorActive()) { edMove(p); return; }
        int oldHot = hotTB;
        hotTB = PtInRect(&rcTransport, p) ? tbAt(p) : -1;
        if (hotTB != oldHot) refresh();

        if (mode == Mode::None) {
            int hc = -1, hw = 0;
            if (PtInRect(&rcLibrary, p) && hasSel()) {
                for (auto& c : cards) if (c.clipId == selClipId) {
                    if (PtInRect(&c.crop, p))         { hc = c.clipId; hw = 1; }
                    else if (PtInRect(&c.savesel, p)) { hc = c.clipId; hw = 2; }
                }
            }
            if (hc != hotSelClip || hw != hotSelBtn) { hotSelClip = hc; hotSelBtn = hw; refresh(); }
            return;
        }
        if (!dragged) {
            if (std::abs(p.x - downPt.x) > S(4) || std::abs(p.y - downPt.y) > S(4)) dragged = true;
        }
        if (mode == Mode::WaveSelect) {
            // Forgiving drag-to-timeline: if the pointer leaves the library and
            // enters the tracks area, treat this as a clip placement drag.
            if (dragged && !PtInRect(&rcLibrary, p) && PtInRect(&rcTimeline, p)) {
                mode = Mode::CardDrag; moveGrabOffset = 0;
                selClipId = -1; selStart = selEnd = 0;
                refresh();
                return;
            }
            const CardLayout* cl = nullptr; for (auto& c : cards) if (c.clipId == dragClipId) cl = &c;
            if (cl) {
                const Clip* c = doc.project().findClip(dragClipId);
                int64_t f = waveFrameAt(*cl, c ? c->frames() : 0, p.x);
                // Anchor-based min/max keeps the selection sorted even when the
                // dragged edge crosses the fixed one (no blink, no swap on up).
                selStart = std::min(waveAnchor, f);
                selEnd   = std::max(waveAnchor, f);
                refresh();
            }
        } else if (mode == Mode::ClipVolume) {
            const CardLayout* cl = nullptr; for (auto& c : cards) if (c.clipId == dragClipId) cl = &c;
            if (cl) if (Clip* c = doc.project().findClip(dragClipId)) { c->gain = sliderGainAt(cl->vol, p.x); refresh(); }
        } else if (mode == Mode::TrackVolume) {
            const TrackLayout* tl = nullptr; for (auto& t : trackLays) if (t.trackId == volTrackId) tl = &t;
            if (tl) if (Track* t = doc.project().findTrack(volTrackId)) { t->gain = sliderGainAt(tl->volRc, p.x); refresh(); }
        } else if (mode == Mode::ZoomDrag) {
            setZoomFrac((float)(p.x - zoomRc.left) / std::max(1, (int)(zoomRc.right - zoomRc.left)));
            clampScroll(); refresh();
        } else if (mode == Mode::TlVScroll) {
            setVScrollFromThumbTop(p.y - vscrollGrab); refresh();
        } else if (mode == Mode::CardDrag || mode == Mode::ClipMove || mode == Mode::TimelineSeek) {
            if (mode == Mode::TimelineSeek) { playheadFrame = xToFrame(p.x); if (timelinePlaying) engine.seek(playheadFrame); }
            refresh();
        }
    }

    void onLUp(POINT p) {
        if (editorActive()) { edUp(p); return; }
        if (GetCapture() == hwnd) ReleaseCapture();
        Mode m = mode; mode = Mode::None;

        if (m == Mode::WaveSelect) {
            if (!dragged && !waveEdgeDrag) {
                // plain click (not on an edge) => seek, clear selection
                selClipId = -1; selStart = selEnd = 0;
                const CardLayout* cl = nullptr; for (auto& c : cards) if (c.clipId == dragClipId) cl = &c;
                if (cl) {
                    const Clip* c = doc.project().findClip(dragClipId);
                    seekClip(dragClipId, waveFrameAt(*cl, c ? c->frames() : 0, p.x));
                }
            }
            // (anchor-based sweep keeps selStart/selEnd sorted; a no-move edge grab
            //  simply leaves the selection unchanged.)
            waveEdgeDrag = false;
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
                } else if (PtInRect(&rcLibrary, p)) {
                    // dropped back inside the library -> reorder to the drop position
                    doc.moveClipInLibrary(dragClipId, libInsertIndex(p));
                    clampScroll(); refresh();
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
    // Add a track and scroll the tracks pane so the new (bottom) track is visible.
    void addTrackAndReveal() { doc.addTrack(); tlScrollY = INT_MAX; clampScroll(); refresh(); }

    void onRDown(POINT p) {
        if (editorActive()) return;
        if (PtInRect(&rcLibrary, p)) {
            const CardLayout* cl = cardAt(p);
            if (cl) { clipContextMenu(cl->clipId, p); return; }
            libraryContextMenu(p); return;
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
                return;
            }
            // right-click a track header or empty lane -> track menu
            for (auto& tl : trackLays) {
                bool onHeader = PtInRect(&tl.header, p);
                bool onLane = PtInRect(&tl.lane, p);
                if (onHeader || onLane) { trackContextMenu(tl.trackId, p); return; }
            }
        }
    }

    void trackContextMenu(int trackId, POINT p) {
        const Track* t = doc.project().findTrack(trackId);
        if (!t) return;
        bool hasClips = !t->clips.empty();
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (hasClips ? 0 : MF_GRAYED), IDM_TRK_DENOISE,
                    L"Voice cleaner \u2014 this track\u2026");
        AppendMenuW(m, MF_STRING | (hasClips ? 0 : MF_GRAYED), IDM_TRK_VOICEISO,
                    L"Remove non-voice \u2014 this track\u2026");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_TRK_RENAME, L"Rename track\u2026");
        AppendMenuW(m, MF_STRING | (doc.project().tracks.size() > 1 ? 0 : MF_GRAYED),
                    IDM_TRK_REMOVE, L"Remove track");
        POINT sp = p; ClientToScreen(hwnd, &sp);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD, sp.x, sp.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == IDM_TRK_DENOISE) voiceCleanTrack(trackId);
        else if (cmd == IDM_TRK_VOICEISO) removeNonVoiceTrack(trackId);
        else if (cmd == IDM_TRK_RENAME) {
            std::wstring nm = t->name;
            if (dlg::promptText(hwnd, L"Rename track", L"Track name:", nm)) { doc.renameTrack(trackId, nm); refresh(); }
        }
        else if (cmd == IDM_TRK_REMOVE) {
            if (doc.project().tracks.size() > 1) { doc.removeTrack(trackId); afterHistory(); }
        }
    }

    // Right-click on the empty library area: sort the clip grid.
    void libraryContextMenu(POINT p) {
        HMENU m = CreatePopupMenu();
        UINT flags = doc.project().library.size() > 1 ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(m, flags, IDM_SORT_NAME, L"Sort clips by name (A\u2013Z)");
        AppendMenuW(m, flags, IDM_SORT_TIME, L"Sort clips by time (oldest first)");
        POINT sp = p; ClientToScreen(hwnd, &sp);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD, sp.x, sp.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == IDM_SORT_NAME) { doc.sortLibrary(true);  clampScroll(); refresh(); }
        else if (cmd == IDM_SORT_TIME) { doc.sortLibrary(false); clampScroll(); refresh(); }
    }

    void clipContextMenu(int clipId, POINT p) {
        HMENU m = CreatePopupMenu();
        bool sel = hasSel() && selClipId == clipId;
        AppendMenuW(m, MF_STRING, IDM_EDIT, L"Open in editor (full window)\u2026");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_PLAY, L"Play / Pause");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_PLAYSEL, L"Play selection");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_SAVESEL, L"Save selection as new clip");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_CROP, L"Crop to selection\u2026");
        // The manual way to get rid of something "Remove non-voice" won't touch.
        // Silencing keeps the clip's length (so a pause between sentences stays
        // the length it was); deleting closes the gap.
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_SILENCESEL,
                    L"Silence selection (keep timing)");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_DELETESEL,
                    L"Delete selection (close gap)");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_CLEARSEL, L"Clear selection");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        // Writing audio back out. Edits only ever live in the .acep, so this is
        // the one way to get an edited clip out as a file; "Save selection as new
        // clip" above is its in-project counterpart. Greyed like the other
        // selection entries so the option reads as available before there is one.
        AppendMenuW(m, MF_STRING, IDM_EXPORTCLIP, L"Export clip to file\u2026");
        AppendMenuW(m, MF_STRING | (sel ? 0 : MF_GRAYED), IDM_EXPORTSEL,
                    L"Export selection to file\u2026");
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
        // The range an effect runs over is picked here rather than inside the
        // dialog, so it's visible before committing to opening one. The selection
        // entry is greyed (not hidden) so it reads as an available option even
        // when nothing is selected yet.
        HMENU vc = CreatePopupMenu();
        AppendMenuW(vc, MF_STRING | (selectionCovers(clipId) ? 0 : MF_GRAYED), IDM_DENOISE_SEL,
                    L"This clip (selection)\u2026");
        AppendMenuW(vc, MF_STRING, IDM_DENOISE, L"This clip\u2026");
        AppendMenuW(vc, MF_STRING, IDM_DENOISE_ALL, L"All clips (whole project)\u2026");
        AppendMenuW(vc, MF_SEPARATOR, 0, nullptr);
        // Always enabled: capturing uses whatever clip currently holds the
        // drag-selection (not necessarily the one right-clicked). If there is no
        // selection yet, the handler explains how to make one.
        AppendMenuW(vc, MF_STRING, IDM_GETPROFILE,
                    hasSel() ? L"Capture noise from selection"
                             : L"Capture noise from selection\u2026");
        AppendMenuW(vc, MF_STRING, IDM_GETPROFILE_CLIP, L"Capture noise from whole clip");
        // Apply a remembered background-noise capture straight to this clip.
        HMENU rec = CreatePopupMenu();
        if (noiseCaptures.empty()) {
            AppendMenuW(rec, MF_STRING | MF_GRAYED, 0, L"(no captures yet \u2014 capture noise from a selection)");
        } else {
            for (size_t i = 0; i < noiseCaptures.size(); ++i)
                AppendMenuW(rec, MF_STRING, IDM_APPLYCAP_BASE + (int)i, noiseCaptures[i].desc.c_str());
        }
        AppendMenuW(vc, MF_POPUP, (UINT_PTR)rec, L"Apply noise capture \u25B8");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)vc, L"Voice cleaner (reduce noise)");
        HMENU vi = CreatePopupMenu();
        AppendMenuW(vi, MF_STRING | (selectionCovers(clipId) ? 0 : MF_GRAYED), IDM_VOICEISO_SEL,
                    L"This clip (selection)\u2026");
        AppendMenuW(vi, MF_STRING, IDM_VOICEISO, L"This clip\u2026");
        AppendMenuW(vi, MF_STRING, IDM_VOICEISO_ALL, L"All clips (whole project)\u2026");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)vi, L"Remove non-voice (bumps, shuffling)");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_RENAME, L"Rename\u2026");
        AppendMenuW(m, MF_STRING, IDM_DELETE, L"Delete clip");

        POINT sp = p; ClientToScreen(hwnd, &sp);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD, sp.x, sp.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 0) return;
        if (cmd == IDM_EDIT) openClipEditor(clipId);
        else if (cmd == IDM_PLAY) togglePlayClip(clipId);
        else if (cmd == IDM_PLAYSEL) { selClipId = clipId; playSelection(); }
        else if (cmd == IDM_SAVESEL) { selClipId = clipId; saveSelectionAsClip(); }
        else if (cmd == IDM_CROP) { selClipId = clipId; cropSelection(); }
        else if (cmd == IDM_SILENCESEL) removeSelectedRange(clipId, true);
        else if (cmd == IDM_DELETESEL) removeSelectedRange(clipId, false);
        else if (cmd == IDM_CLEARSEL) { selClipId = -1; selStart = selEnd = 0; refresh(); }
        else if (cmd == IDM_EXPORTCLIP) exportClipAudio(clipId, false);
        else if (cmd == IDM_EXPORTSEL) exportClipAudio(clipId, true);
        else if (cmd == IDM_NORM_MATCH) normalizeClip(clipId, false);
        else if (cmd == IDM_NORM_ALL) normalizeClip(clipId, true);
        else if (cmd == IDM_DENOISE) voiceCleanClip(clipId);
        else if (cmd == IDM_DENOISE_SEL) voiceCleanClipSelection(clipId);
        else if (cmd == IDM_DENOISE_ALL) voiceCleanAllClips();
        else if (cmd == IDM_GETPROFILE) captureNoiseProfileFromSelection();
        else if (cmd == IDM_GETPROFILE_CLIP) captureNoiseProfileFromClip(clipId);
        else if (cmd == IDM_VOICEISO) removeNonVoiceClip(clipId);
        else if (cmd == IDM_VOICEISO_SEL) removeNonVoiceClipSelection(clipId);
        else if (cmd == IDM_VOICEISO_ALL) removeNonVoiceAllClips();
        else if (cmd >= IDM_APPLYCAP_BASE && cmd < IDM_APPLYCAP_BASE + 100)
            applyCaptureToClip(clipId, cmd - IDM_APPLYCAP_BASE);
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
        case TB_TRACK: addTrackAndReveal(); break;
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
        case IDC_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case IDC_UNDO: doUndo(); break;
        case IDC_REDO: doRedo(); break;
        case IDC_ADDTRACK: addTrackAndReveal(); break;
        case IDC_CONTROLS: showControls(); break;
        }
    }

    // --------------------------------------------------------- keyboard
    // Esc abandons the gesture in progress and puts things back as they were when
    // it started: a selection sweep or edge-nudge restores the previous selection,
    // a clip drag drops nothing. Returns false when no such drag was running, so
    // the caller can fall through to Esc's other meaning (closing the editor).
    //
    // Only gestures with a discrete outcome are cancellable here. The continuous
    // ones -- volume, zoom, the scrollbar -- show their value moving under the
    // cursor as you drag, so there is no half-finished result to abandon.
    bool cancelDrag() {
        if (editorActive()) {
            if (editDrag == 0) return false;    // main sweep, or either fine-tune strip
            editDrag = 0; mainEdgeDrag = false;
        } else {
            // CardDrag is included because a sweep on a card *becomes* one when the
            // pointer leaves the library, and that conversion clears the selection;
            // cancelling has to undo that too, not just skip the drop.
            if (mode != Mode::WaveSelect && mode != Mode::CardDrag && mode != Mode::ClipMove)
                return false;
            mode = Mode::None; waveEdgeDrag = false;
        }
        selClipId = selSaveClipId; selStart = selSaveStart; selEnd = selSaveEnd;
        if (GetCapture() == hwnd) ReleaseCapture();
        dragged = false;
        refresh();
        return true;
    }

    void onKey(WPARAM k) {
        bool ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        bool shift = GetKeyState(VK_SHIFT) & 0x8000;
        if (editorActive()) {
            // Cancelling a drag takes precedence over closing: mid-gesture, Esc
            // means "undo what I'm doing", not "throw away the whole editor".
            if (k == VK_ESCAPE) { if (!cancelDrag()) closeClipEditor(); return; }
            // Space is the universal play/pause, not a third transport: it acts
            // on whatever is armed (so it resumes a paused selection audition as
            // a selection audition) and pauses anything playing, rather than
            // taking over as a whole-clip play the way the Play button would.
            if (k == VK_SPACE) {
                togglePreview(editClipId, previewClipId == editClipId && previewIsSel, false);
                return;
            }
            if (ctrl && (k == 'Z')) { if (shift) doRedo(); else doUndo(); return; }
            return;
        }
        if (k == VK_ESCAPE) { cancelDrag(); return; }
        if (ctrl && (k == 'Z')) { if (shift) doRedo(); else doUndo(); return; }
        if (ctrl && (k == 'Y')) { doRedo(); return; }
        if (ctrl && (k == 'S')) { saveProjectFile(); return; }
        if (ctrl && (k == 'O')) { addFiles(); return; }
        if (k == VK_SPACE) {
            if (previewClipId >= 0 && !timelinePlaying) togglePlayClip(previewClipId);
            else playAll();
            return;
        }
    }

    void onWheel(POINT p, int delta, bool ctrl, bool shift) {
        ScreenToClient(hwnd, &p);
        if (editorActive()) { edWheel(p, delta); return; }
        if (PtInRect(&rcLibrary, p)) {
            libScroll -= delta / 2; clampScroll(); refresh(); return;
        }
        if (PtInRect(&rcTimeline, p)) {
            bool overHeader = p.x < rcTimeline.left + trackHeaderW;
            if (ctrl) {
                double old = pxPerSec;
                pxPerSec *= (delta > 0 ? 1.15 : 1 / 1.15);
                pxPerSec = std::max(8.0, std::min(2000.0, pxPerSec));
                (void)old;
            } else if (shift || overHeader) {
                // Shift+wheel anywhere, or a plain wheel over the track-header
                // column, scrolls the tracks vertically.
                tlScrollY -= delta / 2;
            } else {
                tlScrollX -= delta;
            }
            clampScroll(); refresh();
        }
    }

    // --------------------------------------------------------- timer / playend
    // The playhead is the only thing in the window that animates, so the timer
    // runs fast enough for it to look continuous while something is playing and
    // drops back to the idle housekeeping rate when nothing is. The rate matters
    // most in the fine-tune strips: at their zoom (a few hundred ms across half
    // the window) a 33 ms step is over a hundred pixels, which reads as a
    // stutter however accurate the position underneath it is.
    //
    // The fast period is 15 ms, not 16, on purpose. WM_TIMER fires on the system
    // clock tick, which is ~15.6 ms by default, and a requested period is rounded
    // *up* to a whole number of ticks -- so asking for 16 ms waits two ticks and
    // yields ~31 ms, no better than idle. (Measured: with 16 ms the playhead
    // stepped every 33-35 ms.) Anything at or below one tick fires once per tick,
    // and 15 ms stays sane if some other component has raised the timer
    // resolution: it caps the repaint rate at ~66 Hz instead of running away.
    void setTimerRate(bool playing) {
        if (playing == timerFast) return;
        timerFast = playing;
        SetTimer(hwnd, 1, playing ? 15 : 33, nullptr);
    }

    void onTimer() {
        // Keep the title's unsaved-changes marker (" *") in sync as edits happen.
        if (projectModified() != lastTitleDirty) { lastTitleDirty = projectModified(); setTitle(); }
        const bool playing = engine.isPlaying();
        setTimerRate(playing);
        if (playing) {
            if (timelinePlaying) playheadFrame = engine.position();
            else if (previewClipId >= 0) previewCursor = previewBegin + engine.position();
            refresh();
        }
    }
    void onPlayEnd() {
        // source drained
        if (timelinePlaying) { timelinePlaying = false; }
        else if (previewClipId >= 0) previewCursor = previewEnd;
        refresh();
    }
};

// ----------------------------------------------------------------- window plumbing
static App* g_app = nullptr;
static void saveWindowPlacement(HWND hwnd);   // defined below (registry persistence)

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
        SetTimer(hwnd, 1, 33, nullptr);    // idle rate; onTimer speeds it up while playing
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
    case WM_SETCURSOR:
        if (a && LOWORD(lp) == HTCLIENT) {
            if (a->draggingSelEdge()) { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
            if (a->draggingCard())   { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return TRUE; }
            POINT cp; GetCursorPos(&cp); ScreenToClient(hwnd, &cp);
            if (a->overSelEdge(cp))        { SetCursor(LoadCursor(nullptr, IDC_SIZEWE)); return TRUE; }
            if (a->overCardDragHandle(cp)) { SetCursor(LoadCursor(nullptr, IDC_SIZEALL)); return TRUE; }
        }
        break;
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
    case WM_CLOSE:
        if (a && !a->confirmDiscardChanges()) return 0;  // user cancelled exit
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: saveWindowPlacement(hwnd); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- window size/state persistence (HKCU\Software\AudioClipEditor) ----------
static const wchar_t* kRegKey = L"Software\\AudioClipEditor";

static bool loadWindowPlacement(WINDOWPLACEMENT& wp) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    WINDOWPLACEMENT tmp{}; DWORD sz = sizeof(tmp), type = 0;
    LONG r = RegQueryValueExW(k, L"WindowPlacement", nullptr, &type, (LPBYTE)&tmp, &sz);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_BINARY || sz != sizeof(WINDOWPLACEMENT)) return false;
    // Don't restore into a minimized state; open normally (or maximized) instead.
    if (tmp.showCmd == SW_SHOWMINIMIZED || tmp.showCmd == SW_MINIMIZE)
        tmp.showCmd = SW_SHOWNORMAL;
    wp = tmp; wp.length = sizeof(wp);
    return true;
}

static void saveWindowPlacement(HWND hwnd) {
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"WindowPlacement", 0, REG_BINARY, (const BYTE*)&wp, sizeof(wp));
    RegCloseKey(k);
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
    // Project runs at >= 48 kHz internally regardless of the device mix rate; the
    // engine resamples to the device on playback. Source files are decoded to this
    // rate on load, so clips of any rate/bit depth are unified into the project.
    int devRate = app.engine.sampleRate() > 0 ? app.engine.sampleRate() : 48000;
    app.rate = std::max(48000, devRate);
    app.engine.setSourceRate(app.rate);
    app.doc.init(app.rate);

    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;   // deliver WM_LBUTTONDBLCLK (needed for the clip editor)
    wc.lpfnWndProc = WndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"AudioClipEditorMain";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Audio Clip Editor",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1500, 950,
        nullptr, nullptr, hInst, &app);
    g_app = &app;

    // engine end-of-source -> post to UI thread
    app.engine.setEndCallback([hwnd] { PostMessageW(hwnd, WM_APP_PLAYEND, 0, 0); });

    // Restore the last window size/position/maximized state if we have one.
    WINDOWPLACEMENT wp{};
    if (loadWindowPlacement(wp)) SetWindowPlacement(hwnd, &wp);
    else ShowWindow(hwnd, nCmdShow);
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
