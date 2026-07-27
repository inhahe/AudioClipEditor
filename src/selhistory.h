#pragma once
#include <cstdint>
#include <vector>

// Undo/redo for the waveform selection.
//
// A selection is positioned by hand and one stray click destroys it, so it needs
// to be undoable -- but it is *not* project state, so it has no business in the
// document's snapshot tree: putting it there would copy the whole project per
// drag and fill the redo-branch picker with entries that change no audio.
//
// So it gets its own stack, valid only while the document stays put. Every call
// carries a `base` token identifying where in the document history we are (the
// UI passes the current undo-tree node); when that changes, the stack no longer
// describes reachable states and is dropped. That is also what keeps the
// precedence rule simple to state: Ctrl+Z walks back the selections made since
// the last edit, and once they run out it undoes the edit itself.
//
// Deliberately free of Win32 and of the document, so the whole state machine can
// be exercised headlessly -- see the "selection history" block in selftest.cpp.
namespace selhist {

struct Sel {
    int clipId = -1;
    int64_t start = 0, end = 0;
};

inline bool same(const Sel& a, const Sel& b) {
    return a.clipId == b.clipId && a.start == b.start && a.end == b.end;
}

class History {
public:
    void reset(const void* base) { undo_.clear(); redo_.clear(); base_ = base; }

    bool canUndo(const void* base) const { return base == base_ && !undo_.empty(); }
    bool canRedo(const void* base) const { return base == base_ && !redo_.empty(); }

    // Remember `prev` (the selection before the change that just happened).
    // A gesture that ended where it began records nothing, so a click that
    // re-picks the same range does not leave a dead entry to step through.
    void record(const void* base, const Sel& prev, const Sel& cur) {
        if (base != base_) reset(base);
        if (same(prev, cur)) return;
        undo_.push_back(prev);
        redo_.clear();          // a fresh change abandons the redo branch
    }

    // Callers check canUndo/canRedo first; `cur` is pushed onto the other stack
    // so the step is reversible.
    Sel undo(const Sel& cur) {
        redo_.push_back(cur);
        Sel s = undo_.back(); undo_.pop_back();
        return s;
    }
    Sel redo(const Sel& cur) {
        undo_.push_back(cur);
        Sel s = redo_.back(); redo_.pop_back();
        return s;
    }

    int undoDepth() const { return (int)undo_.size(); }
    int redoDepth() const { return (int)redo_.size(); }

private:
    std::vector<Sel> undo_, redo_;
    const void* base_ = nullptr;
};

} // namespace selhist
