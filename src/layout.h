#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

// Geometry for dropping something into a reflowing, reading-order grid of cards
// (the clip library). These are pure functions of the already-laid-out card
// rectangles, kept out of ui.cpp so they can be regression-tested headlessly --
// see the "library drop" block in selftest.cpp.
namespace layout {

// Reading-order insertion index for a drop at `p` (0..cards.size()). A card comes
// before the cursor if it is on an earlier row, or on the same row and its centre
// is left of the cursor. A point below every row appends.
inline int insertIndex(const std::vector<RECT>& cards, POINT p) {
    for (int i = 0; i < (int)cards.size(); ++i) {
        const RECT& r = cards[i];
        const int cx = (r.left + r.right) / 2;
        const bool earlierRow = p.y < r.top;
        const bool sameRow = p.y >= r.top && p.y < r.bottom;
        if (earlierRow || (sameRow && p.x < cx)) return i;
    }
    return (int)cards.size();
}

// Which card the insertion caret hangs off, and on which side of it.
//
// A row boundary is a single insertion index but two *places*: the empty tail of
// one row and the head of the next. Anchoring the caret always at the head of the
// next row made dropping at the end of a row look impossible -- the caret jumped
// down a row as soon as you aimed there -- so it instead follows whichever of the
// two places the cursor is actually in.
struct CaretAnchor {
    int card = -1;         // index into `cards`, or -1 when there is nothing to draw
    bool trailing = false; // caret sits after that card rather than before it
};

inline CaretAnchor caretAnchor(const std::vector<RECT>& cards, POINT p, int idx) {
    if (cards.empty()) return {};
    const int last = (int)cards.size() - 1;
    if (idx > last) return { last, true };          // appending
    const int prev = idx - 1;
    // Past the last card of a row: `idx` names the first card of the *next* row,
    // but the cursor is still level with the previous one, so trail that instead.
    if (prev >= 0 && cards[prev].top != cards[idx].top &&
        p.y >= cards[prev].top && p.y < cards[prev].bottom)
        return { prev, true };
    return { idx, false };
}

// Flow a row of fixed-width toolbar buttons left to right, wrapping to a new row
// when the next one would not fit. Used by the full-window clip editor's toolbar,
// which stopped fitting on one row: at the default 1500 px window above 125% DPI
// the rightmost buttons ran off the edge and were simply unreachable.
//
// A non-positive entry in `widths` places nothing and advances by `-w` instead,
// which is how a gap between button groups is expressed. `avail` is the x past
// which a button may not extend -- the caller reserves whatever strip its
// right-pinned button (Done) needs.
struct FlowedRow {
    std::vector<RECT> rects;   // one per entry in `widths`; empty for gap entries
    int endX = 0;              // x just past the last button placed
    int endY = 0;              // top of the final row
    int height = 0;            // top of row 0 to the bottom of the toolbar
    int rows = 1;
};

inline FlowedRow flowButtons(const std::vector<int>& widths, int left, int top,
                             int btnH, int gap, int avail, int padBottom) {
    FlowedRow f;
    f.rects.resize(widths.size());
    int bx = left, by = top;
    for (size_t i = 0; i < widths.size(); ++i) {
        const int w = widths[i];
        if (w <= 0) { bx += -w; continue; }
        // Never wrap a row that has placed nothing yet: a button wider than the
        // whole strip has to go somewhere, and an empty new row wouldn't help.
        if (bx > left && bx + w > avail) { bx = left; by += btnH + gap; ++f.rows; }
        f.rects[i] = { bx, by, bx + w, by + btnH };
        bx += w + gap;
    }
    f.endX = bx; f.endY = by; f.height = by + btnH + padBottom;
    return f;
}

// The main transport bar is not a plain flow: it is four groups -- the buttons
// (Add Files, Add Track, Play All, Stop), the mix volume, the position/length
// readout, and the history group (Scale slider, Undo, Redo, History) -- and the
// last one hugs the right edge, which is where the eye expects it and where it has
// always been. What it shares with flowButtons is the rule that nothing may run
// off the edge -- a clipped control is an unusable one, and the readout is the one
// thing on this bar with no menu equivalent, so it may not be the thing that gets
// cut. (It was: before the History button existed the bar happened to fit, and
// adding a fourth right-hand button pushed "0:00.00 / 0:20.00" off the edge
// mid-digit at the default window size.)
//
// So the groups are assigned to rows greedily: each one stays on the current
// row if it fits after what is already there, and drops to a new row, at the left
// margin, if it doesn't. The buttons are always first on row 0; the history group
// is always right-aligned on whatever row it lands on.
struct TransportFlow {
    int mixX = 0;        // left edge of the mix volume group
    int mixRow = 0;      // 0-based row index of the mix volume group
    int midX = 0;        // left edge of the readout
    int midRow = 0;      // ditto for its row
    int rightX = 0;      // left edge of the right-aligned group
    int rightRow = 0;    // ditto for its row
    int rows = 1;
};

inline TransportFlow flowTransport(int width, int margin, int gap, int leftW,
                                   int mixGap, int mixW, int midGap, int midW, int rightW) {
    TransportFlow f;
    const int limit = width - margin;      // nothing may extend past this
    int row = 0;
    int used = margin + leftW;             // rightmost x occupied on the current row

    // Never wrap off an empty row: a group wider than the whole bar has to go
    // somewhere, and a fresh row would not make it fit either.
    auto follow = [&](int groupGap, int w, int& x, int& r) {
        x = used + groupGap;
        if (x + w > limit && used > margin) { ++row; x = margin; }
        r = row;
        used = x + w;
    };
    follow(mixGap, mixW, f.mixX, f.mixRow);
    follow(midGap, midW, f.midX, f.midRow);

    // Right-aligned, but never off the left edge: on an absurdly narrow window
    // the group starts at the margin and the outermost button is the one that
    // overhangs, rather than Scale and Undo disappearing past x = 0.
    f.rightX = limit - rightW;
    if (f.rightX < margin) f.rightX = margin;
    if (f.rightX < used + gap) ++row;
    f.rightRow = row;

    f.rows = row + 1;
    return f;
}

// The main transport bar's measurements at UI scale `sc` (S(v) = v * sc, rounded,
// as App::S does): one definition, read by App::layoutTransport to place the bar and
// by the selftest to check how it wraps and that its labels fit, so what the test
// checks is what is drawn.
struct TransportMetrics {
    int y0 = 0, h = 0;                  // top of row 0, and every row's height
    int gap = 0, margin = 0;            // between controls, and at the bar's ends
    // The buttons: + Add Files, + Add Track, (playGap) Play All, Stop.
    int addW = 0, trackW = 0, playGap = 0, playW = 0, stopW = 0;
    // The mix volume: caption, (capGap) slider, (pctGap) readout.
    int mixGap = 0, mixLabW = 0, capGap = 0, mixSliderW = 0, pctGap = 0, mixPctW = 0;
    int timeGap = 0, timeW = 0;         // the readout: wide enough for "12:34.56  /  12:34.56"
    // The history group: Scale caption, (capGap) slider, (undoGap) Undo, Redo, History.
    int scaleLabW = 0, zoomW = 0, undoGap = 0, undoW = 0, redoW = 0, historyW = 0;
    int sliderH = 0;                    // a slider's height, centred on its row
    // Each group's whole width, as flowTransport takes it.
    int leftW = 0, mixW = 0, rightW = 0;
};

inline TransportMetrics transportMetrics(float sc) {
    auto S = [sc](int v) { return (int)(v * sc + 0.5f); };
    TransportMetrics m;
    m.y0 = S(12); m.h = S(38); m.gap = S(8); m.margin = S(12);
    m.addW = S(96); m.trackW = S(96); m.playGap = m.gap + S(16); m.playW = S(140); m.stopW = S(70);
    m.mixGap = S(20); m.mixLabW = S(66); m.capGap = S(4); m.mixSliderW = S(100);
    m.pctGap = S(6); m.mixPctW = S(40);
    m.timeGap = S(16); m.timeW = S(190);
    m.scaleLabW = S(46); m.zoomW = S(120); m.undoGap = S(12);
    m.undoW = S(70); m.redoW = S(70); m.historyW = S(118);
    m.sliderH = S(14);
    m.leftW = m.addW + m.gap + m.trackW + m.playGap + m.playW + m.gap + m.stopW;
    m.mixW = m.mixLabW + m.capGap + m.mixSliderW + m.pctGap + m.mixPctW;
    m.rightW = m.scaleLabW + m.capGap + m.zoomW + m.undoGap +
               m.undoW + m.gap + m.redoW + m.gap + m.historyW;
    return m;
}

// ---------------------------------------------------------------- scrollbars
//
// The tracks pane can overflow in both directions at once, and its two
// scrollbars share all of this arithmetic, so it lives here once rather than
// twice in ui.cpp -- and can be checked headlessly, which matters because the
// failure mode (content quietly clipped under a bar, or a thumb that does not
// reach the end) is invisible until someone happens to build a long enough
// arrangement.

// Which bars a viewport needs. Resolved over two passes because each bar steals
// space from the other: a horizontal bar shortens the pane and can be exactly
// what forces a vertical one, and vice versa. One pass would miss that and leave
// a strip of content unreachable.
struct ScrollBars { bool horz = false, vert = false; };

inline ScrollBars scrollBarsNeeded(int contentW, int contentH,
                                   int fullW, int fullH, int thickness) {
    ScrollBars b;
    for (int pass = 0; pass < 2; ++pass) {
        const int visH = fullH - (b.horz ? thickness : 0);
        const int visW = fullW - (b.vert ? thickness : 0);
        b.horz = contentW > visW;
        b.vert = visH > 0 && contentH > visH;
    }
    return b;
}

// Thumb offset and length along a gutter of `trackLen`. `minThumb` keeps a thumb
// on a very long arrangement from shrinking to something unclickable -- which is
// why the offset is scaled by the *remaining* travel rather than by trackLen.
struct ScrollThumb { int offset = 0, length = 0; };

inline ScrollThumb scrollThumb(int scrollPos, int contentLen, int visLen,
                               int trackLen, int minThumb) {
    ScrollThumb t;
    if (contentLen <= 0 || trackLen <= 0) return t;
    t.length = (int)((double)visLen / contentLen * trackLen);
    if (t.length < minThumb) t.length = minThumb;
    if (t.length > trackLen) t.length = trackLen;
    const int maxScroll = contentLen - visLen > 0 ? contentLen - visLen : 1;
    const int travel = trackLen - t.length;
    t.offset = travel > 0 ? (int)((double)scrollPos / maxScroll * travel) : 0;
    if (t.offset < 0) t.offset = 0;
    if (t.offset > travel) t.offset = travel;
    return t;
}

// Inverse of scrollThumb: a thumb dragged to `thumbOffset` means this much
// scroll. Clamped, since the pointer can be dragged past either end of the
// gutter and the caller has no reason to pre-clamp it.
inline int scrollFromThumb(int thumbOffset, int trackLen, int thumbLen, int maxScroll) {
    const int travel = trackLen - thumbLen;
    if (travel <= 0 || maxScroll <= 0) return 0;
    double frac = (double)thumbOffset / travel;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    return (int)(frac * maxScroll);
}

// Scroll position that brings the span [x0, x1) into view, keeping `margin` of
// clear space at whichever edge it had to come in from, and leaving the position
// alone when the span is already comfortably inside the viewport -- a jump the
// user did not need is its own kind of confusing.
//
// A span too long to fit lines up its *start*: no scroll position shows all of
// it, and the start is where it begins and where the next edit will be. Both
// coordinates are in content space (0 = start of the content, not of the
// viewport), which is what `scrollPos` measures too.
inline int scrollToReveal(int scrollPos, int visLen, int x0, int x1,
                          int margin, int maxScroll) {
    if (visLen <= 0) return scrollPos;
    // A viewport too small for two margins would keep re-centring on nothing;
    // drop the margin rather than the reveal.
    if (margin * 2 >= visLen) margin = 0;
    int want = scrollPos;
    if (x1 - x0 > visLen - margin * 2)      want = x0 - margin;
    else if (x0 < scrollPos + margin)       want = x0 - margin;
    else if (x1 > scrollPos + visLen - margin) want = x1 - visLen + margin;
    else return scrollPos;
    if (want < 0) want = 0;
    if (want > maxScroll) want = maxScroll;
    return want;
}

// A library card's volume row, and the narrowest a card may be, at UI scale `sc`
// (S as in transportMetrics), and where its parts go (cardRects). A card is as wide as
// its clip is long, but its volume slider is one length on every card -- the length
// of the track headers', which are placed from the same numbers -- so the same drag
// is the same change of volume whichever clip or track it is on, and a short clip's
// slider is not too short to set. The narrowest card is the one that holds that row
// (the speaker, the slider, and the readout after it at its widest, "1600%"), the
// Crop / New clip buttons a selection puts over the waveform, and the title row (play,
// stop, the start of the name, the x).
struct CardMetrics {
    int h = 0, pad = 0;       // every card's height, and the space between cards
    int titleH = 0;           // the title row along the top (name; where a drag starts)
    int playInset = 0, delInset = 0;  // the play button (left) and the x (right), in from the sides
    int btnTop = 0, btnSize = 0;      // all three buttons (play, stop, x): down from the top, and their size
    int stopGap = 0;                  // the stop button, this far right of play
    int nameGap = 0, nameTop = 0;     // the name, this far right of stop, and down from the top
    int waveTop = 0, waveBottom = 0;   // the waveform, down from the top / up from the bottom
    int waveInset = 0;        // ...and in from the sides
    int volLeft = 0;          // the slider's left edge, from the card's; the speaker is before it
    int volW = 0;             // the slider's length, on every card
    int pctGap = 0, pctW = 0; // the readout, right after the slider
    int rightPad = 0;         // from the readout to the card's right edge, at its narrowest
    int rowTop = 0, rowBottom = 0;   // the slider's band, measured up from the card's bottom
    int readoutOverhang = 0;  // the readout's band reaches this far past the slider's
    // The selection's buttons, at the waveform's top right: New clip at the right,
    // Crop before it.
    int selInset = 0, selTop = 0, selH = 0, selGap = 0, saveW = 0, cropW = 0;
    int minW = 0;             // the narrowest card
    // A track header holds the same slider at the same inset, so it is as wide as
    // that and a margin: lengthening the slider widens the header with it.
    int headerW = 0;
};

inline CardMetrics cardMetrics(float sc) {
    auto S = [sc](int v) { return (int)(v * sc + 0.5f); };
    CardMetrics m;
    m.h = S(120); m.pad = S(12); m.titleH = S(24);
    m.playInset = S(6); m.stopGap = S(4); m.delInset = S(4); m.btnTop = S(4); m.btnSize = S(16);
    m.nameGap = S(6); m.nameTop = S(2);
    m.waveTop = S(28); m.waveBottom = S(30); m.waveInset = S(8);
    m.volLeft = S(34); m.volW = S(84); m.pctGap = S(6); m.pctW = S(40); m.rightPad = S(8);
    m.rowTop = S(24); m.rowBottom = S(8); m.readoutOverhang = S(2);
    m.selInset = S(4); m.selTop = S(3); m.selH = S(17); m.selGap = S(4);
    m.saveW = S(58); m.cropW = S(42);
    const int volumeRow = m.volLeft + m.volW + m.pctGap + m.pctW + m.rightPad;
    const int buttons = 2 * m.waveInset + m.selInset + m.saveW + m.selGap + m.cropW;
    // play, stop, a name's beginning (S(40), enough to tell clips apart), and the x
    const int titleRow = m.playInset + m.btnSize + m.stopGap + m.btnSize + m.nameGap + S(40) +
                         m.nameGap + m.btnSize + m.delInset;
    m.minW = std::max(volumeRow, std::max(buttons, titleRow));
    m.headerW = m.volLeft + m.volW + S(10);
    return m;
}

// Where everything on `card` goes. The volume slider and its readout are the same
// length and in the same place on every card, whatever its width.
struct CardRects {
    RECT top, play, stop, del, wave;
    RECT name;                 // the clip's name, between the stop button and the x
    RECT vol, volPct;          // the volume slider, and its readout
    RECT crop, savesel;        // the selection's Crop and New clip buttons
};
inline CardRects cardRects(const CardMetrics& m, const RECT& card) {
    CardRects r;
    const int x = card.left, y = card.top, right = card.right, bottom = card.bottom;
    r.top  = { x, y, right, y + m.titleH };
    r.play = { x + m.playInset, y + m.btnTop, x + m.playInset + m.btnSize, y + m.btnTop + m.btnSize };
    r.stop = { r.play.right + m.stopGap, r.play.top, r.play.right + m.stopGap + m.btnSize, r.play.bottom };
    r.del  = { right - m.delInset - m.btnSize, y + m.btnTop, right - m.delInset, y + m.btnTop + m.btnSize };
    r.name = { r.stop.right + m.nameGap, y + m.nameTop, r.del.left - m.nameGap, y + m.titleH };
    r.wave = { x + m.waveInset, y + m.waveTop, right - m.waveInset, bottom - m.waveBottom };
    r.vol  = { x + m.volLeft, bottom - m.rowTop, x + m.volLeft + m.volW, bottom - m.rowBottom };
    r.volPct = { r.vol.right + m.pctGap, r.vol.top - m.readoutOverhang,
                 r.vol.right + m.pctGap + m.pctW, r.vol.bottom + m.readoutOverhang };
    const int by = r.wave.top + m.selTop, bx = r.wave.right - m.selInset;
    r.savesel = { bx - m.saveW, by, bx, by + m.selH };
    r.crop = { r.savesel.left - m.selGap - m.cropW, by, r.savesel.left - m.selGap, by + m.selH };
    return r;
}

// The volume slider's scale, on cards and track headers alike: slider position
// 0..1 <-> linear gain. The left half is 0..100%, linear. The right half runs from
// 100% up to `maxGain` (the most a volume can be, +24 dB), with dB rising as the
// square of the distance along it, so 100..200% -- where hand adjustment happens --
// still has half of that side to itself. The point is that every volume a clip or
// track can have has a position: volume matching sets up to +24 dB, and a scale
// that stopped short would pin the knob at its end and turn a click on it into a
// drop of everything above.
inline float volumeToFrac(float gain, float maxGain) {
    if (!(gain > 1.0f)) return std::max(0.0f, gain) * 0.5f;
    const float u = std::sqrt(std::min(1.0f, std::log(gain) / std::log(maxGain)));
    return 0.5f + 0.5f * u;
}
inline float fracToVolume(float frac, float maxGain) {
    frac = std::max(0.0f, std::min(1.0f, frac));
    if (frac <= 0.5f) return frac * 2.0f;
    const float u = (frac - 0.5f) * 2.0f;
    return std::min(maxGain, std::pow(maxGain, u * u));
}

}  // namespace layout
