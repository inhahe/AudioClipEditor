#pragma once
#include <cstdint>
#include <cstdlib>
#include <vector>

// Directional ("sticky") snapping for dragging clips along a track.
//
// Ordinary snapping is symmetric: whenever the dragged position comes within a
// few pixels of an interesting frame it is pulled onto it. That is convenient,
// but it makes a whole band around every snap point *unreachable* -- you cannot
// leave a 3-frame gap after a neighbour, because the moment you get that close
// the clip jumps flush against it. The gap you want and the snap you want look
// identical to the drag, and the snap always wins.
//
// So the pull is applied in one direction only:
//
//   * Moving *toward* a snap point: nothing happens. The clip tracks the mouse
//     exactly, so you can stop arbitrarily close to a neighbour -- one frame
//     away if you like -- and that is what you get.
//   * Reaching or crossing a snap point engages it: the clip parks exactly on
//     it. This is how you land flush, and it needs no aim, since anywhere past
//     the point does it.
//   * Moving *away* from an engaged point: the clip stays put, resisting, until
//     the mouse is more than `tol` from it -- then it lets go and jumps to the
//     mouse.
//
// So snapping still costs nothing to use (overshoot slightly and you are flush,
// which is the common case), while every position remains reachable: approach
// from the far side, where there is no pull. The one deliberate cost is that
// pulling free jumps by `tol` -- that pop is the feedback that you have left the
// snap, and there is no drift, since the clip is back on the mouse afterwards.
//
// Deliberately free of Win32 and of the document, so the state machine can be
// exercised headlessly -- see the "sticky snapping" block in selftest.cpp.
namespace snapping {

class Sticky {
public:
    // Call once when the drag starts, with the position the clip is already at.
    // Passing the real starting position (rather than 0) is what makes a clip
    // that is *already* flush against a neighbour resist the first nudge, the
    // same way it would if you had just dragged it there.
    void begin(int64_t at) { prev_ = at; stuck_ = false; stuckAt_ = 0; }

    // `raw` is where the mouse says the clip's start should be, `targets` the
    // frames worth snapping to (neighbour edges, timeline start), `tol` how far
    // the mouse may stray from an engaged point before it lets go.
    //
    // Must be called exactly once per pointer update -- it advances the state
    // machine, so painting has to use the stored result rather than re-deriving
    // it. Idempotent for a repeated `raw`, so a mouse-up at the last move's
    // position is harmless.
    int64_t update(int64_t raw, const std::vector<int64_t>& targets, int64_t tol) {
        if (stuck_) {
            if (std::llabs(raw - stuckAt_) <= tol) { prev_ = raw; return stuckAt_; }
            stuck_ = false;      // pulled far enough away: let go
        }
        // Not engaged, so the only way to engage is to have arrived at a target
        // or passed over one during this step. Approaching one exerts no pull at
        // all, which is the whole point.
        const int64_t lo = prev_ < raw ? prev_ : raw;
        const int64_t hi = prev_ < raw ? raw : prev_;
        bool found = false; int64_t best = 0, bestD = 0;
        for (int64_t t : targets) {
            if (t < lo || t > hi) continue;          // neither reached nor crossed
            const int64_t d = std::llabs(raw - t);
            // A fast drag can sweep over a target and end far past it; that is a
            // move *through*, not a landing, and grabbing it would leave the clip
            // lagging way behind the mouse.
            if (d > tol) continue;
            if (!found || d < bestD) { found = true; bestD = d; best = t; }
        }
        prev_ = raw;
        if (!found) return raw;
        stuck_ = true; stuckAt_ = best;
        return best;
    }

    // True while parked on a snap point (so the UI can say so).
    bool engaged() const { return stuck_; }
    int64_t engagedAt() const { return stuckAt_; }

private:
    int64_t prev_ = 0;       // last raw position, for detecting a crossing
    bool    stuck_ = false;
    int64_t stuckAt_ = 0;
};

} // namespace snapping
