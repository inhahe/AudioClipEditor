#pragma once
#include <windows.h>
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

}  // namespace layout
