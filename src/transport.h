#pragma once
#include <cstdint>

// What a press of a play/pause control should do to the clip-preview engine.
//
// This is pure decision logic with no Win32 or engine dependency, kept out of
// ui.cpp so it can be regression-tested headlessly -- see the "preview transport"
// block in selftest.cpp. It exists because the rule is genuinely subtle: there
// are two ranges a clip can be auditioned over (the whole clip and the
// selection), two kinds of caller (a single combined button, and the editor's
// pair of independent buttons), and three possible outcomes.
namespace transport {

enum class Act {
    Pause,      // something is playing that this control owns -> stop it in place
    Resume,     // the armed source already covers what was asked for -> continue
    Restart,    // re-arm a source and start it (see From for where)
};

// Where a Restart begins.
enum class From {
    Cursor,     // carry on from wherever the play cursor currently sits
    SelStart,   // the head of the selection: a fresh selection audition
    ClipStart,  // the head of the clip: a clip that wasn't armed at all
};

struct Plan { Act act; From from; };

// The engine/preview state at the moment of the press.
struct State {
    int     clipId      = -1;      // clip the armed preview source belongs to
    bool    isSel       = false;   // that source spans the selection, not the clip
    int64_t begin       = 0;       // the armed source's range, in clip frames
    int64_t end         = 0;
    bool    playing     = false;
    bool    paused      = false;
    bool    seekPending = false;   // cursor moved while stopped: must re-arm
    bool    timeline    = false;   // the timeline, not a preview, owns the engine
};

// The press itself.
struct Press {
    int     clipId       = -1;
    bool    wantSel      = false;  // asks for the selection audition, and the
                                   // clip really does have a selection
    // `ownRangeOnly` is what tells the two kinds of control apart. A single
    // combined play/pause button (the library card, the Space key) stands for
    // whatever is playing, so it pauses any running preview of the clip: false.
    // The editor's two buttons are each labelled for one range and each show
    // Pause only while *that* range is playing, so pressing one while the other
    // is running must switch playback to it rather than pause -- otherwise the
    // neighbouring button appears to react to a press it never received: true.
    bool    ownRangeOnly = false;
    int64_t selBegin     = 0;      // the current selection, when wantSel
    int64_t selEnd       = 0;
};

inline Plan decide(const State& s, const Press& p) {
    const bool sameClip = s.clipId == p.clipId && !s.timeline;
    // The armed source already covers exactly the range asked for, so playback
    // can continue rather than re-arm.
    const bool sameSource = sameClip && s.isSel == p.wantSel &&
                            (!p.wantSel || (s.begin == p.selBegin && s.end == p.selEnd));

    if (sameClip && s.playing && (!p.ownRangeOnly || s.isSel == p.wantSel))
        return { Act::Pause, From::Cursor };
    if (sameSource && s.paused && !s.seekPending)
        return { Act::Resume, From::Cursor };

    // A selection audition resumes at the cursor only when it is continuing the
    // very same audition (paused with a pending seek, or stopped at the end);
    // any other press starts it at the selection head. A whole-clip play carries
    // on from the cursor, unless the clip wasn't the armed one at all.
    From from = p.wantSel ? (sameSource ? From::Cursor : From::SelStart)
                          : (s.clipId == p.clipId ? From::Cursor : From::ClipStart);
    return { Act::Restart, from };
}

} // namespace transport
