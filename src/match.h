#pragma once
// Making clips alike: in volume, or in tone colour.
//
// The two matchers differ only in what they measure and what they write back --
// a clip's speech level, written as its volume; a clip's long-term spectrum,
// written as one EQ curve baked into its audio. Everything else is the same
// question asked twice: which clips are involved, which of them change, what they
// are matched to, and how close they ended up. That shared part lives here, out of
// ui.cpp, so the two cannot drift apart ("this track" meaning one thing to one of
// them and something else to the other) and so the selftest can reach it.
//
// Both write **one setting per clip**: one gain, or one static EQ curve applied
// identically from the first sample to the last. Nothing here adapts over the
// course of a clip -- a level or tone change *inside* a take is the performance,
// and survives the match.
#include "model.h"
#include "dsp.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace match {

// The clips one "make these alike" command involves, before any audio is measured.
//
// It has one of two shapes:
//  - a *set* ("make these alike"): every clip is matched, and every clip is in the
//    pool;
//  - *one clip to the rest* ("make this one like the others"): one clip is matched,
//    and the pool is the others, which are left exactly as they are.
//
// The pool is what "the average" is taken over, and the clips one may pick as the
// reference ("sound like 'take 2'"). In the second shape the matched clip is kept
// out of its own average; otherwise matching it would only move it part of the way.
struct ClipSet {
    std::vector<int> ids;               // display order, no duplicates, all with audio
    std::vector<std::wstring> names;    // parallel to ids
    std::vector<uint8_t> change;        // 1 = matched: its volume or audio is rewritten
    std::vector<uint8_t> pool;          // 1 = in the average, and may be the reference
    std::wstring what;                  // the clips that change: "track 'Voice'", "'take 5'"
    std::wstring others;                // single() only, the pool: "the other 7 clips on 'Voice'"
    std::wstring label;                 // the dialog's "Applies to:" line, with counts

    int size() const { return (int)ids.size(); }
    int changeCount() const;
    int poolCount() const;
    // One clip matched to the rest, rather than a set matched to each other.
    bool single() const { return changeCount() == 1 && poolCount() > 0 && !pool[firstChanged()]; }
    int firstChanged() const;           // index of the first matched clip, or -1
    int indexOf(int clipId) const;      // -1 if the clip isn't in the set
    // Whether the command can do anything: a set needs two clips, and one clip to
    // the rest needs at least one other. Menus grey their entry when it can't.
    bool usable() const;
    // Set indices of the pool, in order: the clips a dialog offers as references.
    std::vector<int> poolIndices() const;
    // The reference to preselect: `clipId`'s set index if it is in the pool, else -1
    // (the average). The remembered choice is a clip id, not a list position: the
    // list differs from one command to the next, and a position from last time would
    // quietly come to mean another clip.
    int referenceFor(int clipId) const;
};

// Every distinct clip placed on the track.
ClipSet trackSet(const Project& p, int trackId);
// Every distinct clip placed on any track: "the track or tracks", without the
// library's unplaced clips (a source recording, an outtake, a noise sample).
ClipSet timelineSet(const Project& p);
// Every clip in the library.
ClipSet librarySet(const Project& p);
// `clipId` matched to the other clips on the track it sits on.
ClipSet clipToTrack(const Project& p, int clipId, int trackId);
// `clipId` matched to every other clip in the library.
ClipSet clipToLibrary(const Project& p, int clipId);

// How far apart the clips are, by whichever measure the matcher uses (dB of level,
// or dB of tone colour from dsp::timbreDistanceDb). For a set it is the two clips
// furthest apart -- the worst pair, not an average, because an average hides the one
// stubborn clip among five that agree, and that clip is exactly the one the ear picks
// out. For one clip to the rest it is that clip's distance from what it was matched
// to. `a` and `b` index the ClipSet; `b == -1` means "the average".
struct Spread {
    double db = -1.0;
    int a = -1, b = -1;
    bool valid() const { return db >= 0.0; }
};

// ---------------------------------------------------------------- volume
// The limits a matched volume is held to. The top is the most any clip volume can
// be (Clip::gain is capped there everywhere); the bottom is -26 dB, past which a
// clip is not "a bit louder than the others" but a different kind of recording.
constexpr float kMinMatchGain = 0.05f;
constexpr float kMaxMatchGain = kMaxClipGain;

struct VolumeResult {
    bool ok = false;                    // false: what it was to match had no speech in it
    int changed = 0;                    // clips whose volume actually moves
    std::vector<std::pair<int, float>> gains;   // (clip id, new volume) for the matched clips
    // Parallel to the set:
    std::vector<uint8_t> measured;      // had speech to measure
    std::vector<double> levelDb;        // speech level at its current volume, dB FS
    std::vector<float> oldGain, newGain;
    double targetDb = 0.0;              // the level the matched clips were set to
    int unmeasured = 0;                 // clips left out: silent
    std::vector<int> held;              // matched clips stopped at a volume limit
    // Matched clips whose peaks now go over full scale -- in the mix, at the volume
    // of the loudest track they are on -- so they will distort. (set index, dB over)
    std::vector<std::pair<int, double>> overFullScale;
    // The tracks these clips sit on, when their volumes differ: track volume applies
    // on top of clip volume, so clips matched here still differ in the mix by that.
    std::vector<std::pair<std::wstring, float>> trackGains;
    Spread before, after;
    int biggest = -1;                   // the matched clip whose volume moved most
    double biggestDb = 0.0;             // ...and by how much (signed)
};

// Set each matched clip's volume so its speech level equals the target: the
// reference clip's level if `reference` (a set index from the pool) is given,
// otherwise the geometric mean of the pool's levels -- the target that moves the
// clips the least and favours none of them. Levels are measured over speech only
// (dsp::speechLoudness), at each clip's *current* volume, so the reference keeps the
// volume it has and a clip already at the target stays put. Commits nothing.
VolumeResult matchVolume(const Project& p, const ClipSet& s, int reference);

// ---------------------------------------------------------------- timbre
struct TimbreResult {
    bool ok = false;                    // false: the target couldn't be measured
    std::vector<std::pair<int, AudioBufferPtr>> updates;   // for Document::replaceClipBuffers
    int rate = 0;                       // the rate every measured clip shares
    int unmeasured = 0;                 // clips left out: too short or silent
    int mismatched = 0;                 // clips left out: a different sample rate
    double worstDb = 0.0;               // the largest correction applied, dB...
    double worstHz = 0.0;               // ...at what frequency...
    int worstClip = -1;                 // ...and to which clip (set index)
    Spread before, after;
};

// Measure every clip in the set, decide the target -- the reference clip's tone
// colour, or the average of the pool's -- and filter each matched clip onto it with
// dsp::matchTimbre. Everything is measured before anything is filtered, since the
// target may be an average of them. The reference clip is not processed at all: it
// is the standard, so it stays bit-for-bit what it was. Then every result is
// measured again for the before/after spread. Commits nothing.
TimbreResult matchTimbre(const Project& p, const ClipSet& s, int reference,
                         const dsp::TimbreMatchOptions& opts);

} // namespace match
