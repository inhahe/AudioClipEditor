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

// The main transport bar is not a plain flow: it is three groups, and the last
// one hugs the right edge (Scale slider, Undo, Redo, History), which is where
// the eye expects it and where it has always been. What it shares with
// flowButtons is the rule that nothing may run off the edge -- a clipped
// control is an unusable one, and the position/length readout in the middle
// group is the one thing on this bar with no menu equivalent, so it may not be
// the thing that gets cut. (It was: before the History button existed the bar
// happened to fit, and adding a fourth right-hand button pushed "0:00.00 /
// 0:20.00" off the edge mid-digit at the default window size.)
//
// So the groups are assigned to rows greedily: each one stays on the current
// row if it fits after what is already there, and drops to a new row if it
// doesn't. The left group is always first on row 0; the right group is always
// right-aligned on whatever row it lands on; the middle group starts after the
// left group when they share a row and at the left margin when it has wrapped.
struct TransportFlow {
    int midX = 0;        // left edge of the middle group
    int midRow = 0;      // 0-based row index of the middle group
    int rightX = 0;      // left edge of the right-aligned group
    int rightRow = 0;    // ditto for its row
    int rows = 1;
};

inline TransportFlow flowTransport(int width, int margin, int gap,
                                   int leftW, int midGap, int midW, int rightW) {
    TransportFlow f;
    const int limit = width - margin;      // nothing may extend past this
    int row = 0;
    int used = margin + leftW;             // rightmost x occupied on the current row

    f.midX = used + midGap;
    // Never wrap off an empty row: a group wider than the whole bar has to go
    // somewhere, and a fresh row would not make it fit either.
    if (f.midX + midW > limit && leftW > 0) {
        row = 1;
        f.midX = margin;
        used = margin + midW;
    } else {
        used = f.midX + midW;
    }
    f.midRow = row;

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

}  // namespace layout
