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

}  // namespace layout
