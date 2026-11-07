#pragma once
#include "audio_buffer.h"
#include <string>
#include <vector>
#include <algorithm>

// The most any clip or track volume can be (+24 dB). One constant, because the
// volume matcher reports when a clip was held at this limit, and that report is only
// true if it is the same limit Document enforces.
constexpr float kMaxClipGain = 16.0f;

// A clip's timbre EQ: one fixed gain per frequency bin, in dB, over the bins of
// the timbre analysis (dsp::timbreBins). Shared and immutable, so the undo history's
// snapshots of a clip cost a pointer each, not a copy of the curve.
using EqCurve = std::shared_ptr<const std::vector<float>>;

// A named piece of audio in the clip library. Each clip owns its own AudioBuffer
// (shared_ptr, immutable once created). Derived clips (from a selection/crop) get
// their own trimmed copy so they're fully independent.
struct Clip {
    int id = 0;
    std::wstring name;
    std::wstring sourcePath;        // original file on disk, if any
    // The clip's audio *before* its EQ: what edits (crop, clean, silence, ...) work
    // on and what the project file stores. Anything that plays, draws, measures or
    // exports the clip wants heard() instead.
    AudioBufferPtr buffer;
    // The EQ timbre matching set (null = none), and `buffer` through it. A setting,
    // not rewritten samples: a later match replaces the curve rather than stacking
    // on it, and the EQ can be taken off again. `eqBuffer` is derived -- Document
    // renders it whenever the curve or the audio changes -- and is null when there
    // is no EQ, or until a restored history state is rendered.
    EqCurve eq;
    AudioBufferPtr eqBuffer;
    PeakCachePtr peaks;             // of heard(): the waveform shows what plays
    float gain = 1.0f;             // linear volume multiplier

    // What the clip sounds like: its audio through its EQ.
    const AudioBufferPtr& heard() const { return eq && eqBuffer ? eqBuffer : buffer; }
    bool hasEq() const { return eq != nullptr; }
    uint64_t timestamp = 0;        // FILETIME ticks: source file's mtime for loaded
                                   // clips, else the wall-clock time it was created.
                                   // Used for "sort by time" in the library.

    int64_t frames() const { return buffer ? buffer->frames() : 0; }
    int sampleRate() const { return buffer ? buffer->sampleRate : 48000; }
    double durationSec() const { return buffer ? buffer->durationSec() : 0.0; }
};

// A change to one clip's EQ, as a match produces it: the new curve (null takes
// the EQ off) and, when it has already been rendered, the clip's audio through it.
struct ClipEq {
    int clipId = -1;
    EqCurve eq;
    AudioBufferPtr heard;           // may be null: Document renders it
};

// A clip instance placed on a track at a timeline position (in project frames).
struct PlacedClip {
    int clipId = 0;
    int64_t startFrame = 0;         // timeline position
    int64_t lengthFrames = 0;       // cached from the clip at placement time
    int64_t endFrame() const { return startFrame + lengthFrames; }
};

struct Track {
    int id = 0;
    std::wstring name;
    std::vector<PlacedClip> clips;  // kept sorted by startFrame, never overlapping
    bool muted = false;
    float gain = 1.0f;             // linear volume multiplier

    void sortClips() {
        std::sort(clips.begin(), clips.end(),
            [](const PlacedClip& a, const PlacedClip& b) { return a.startFrame < b.startFrame; });
    }
    // The space in front of clip `index`: from the end of the clip before it, or
    // from the start of the timeline if it is the first. Never negative, since
    // clips on a track never overlap.
    int64_t gapBefore(int index) const {
        if (index < 0 || index >= (int)clips.size()) return 0;
        return clips[index].startFrame - (index > 0 ? clips[index - 1].endFrame() : 0);
    }

    // Slide clip `index` and every clip after it by `delta` frames, keeping the
    // distances *between* them exactly as they are -- a "ripple". Changing one
    // gap therefore leaves every later gap alone, which is the whole point:
    // moving one clip normally would eat into or open up the next gap too.
    //
    // Returns the delta actually applied. Sliding left stops once the block is
    // flush against the clip in front (or the start of the timeline), because
    // there is nowhere further to go without overlapping; sliding right is
    // unbounded, as the timeline has no end.
    //
    // Translating a whole suffix rigidly, with that clamp keeping it behind the
    // prefix, cannot reorder or overlap anything -- so unlike moveClip this needs
    // no collision test and no re-sort.
    int64_t ripple(int index, int64_t delta) {
        if (index < 0 || index >= (int)clips.size()) return 0;
        delta = std::max(delta, -gapBefore(index));
        if (delta == 0) return 0;
        for (int i = index; i < (int)clips.size(); ++i) clips[i].startFrame += delta;
        return delta;
    }

    // Would placing [start,start+len) collide with an existing clip (ignoring index `ignore`)?
    bool overlaps(int64_t start, int64_t len, int ignore = -1) const {
        int64_t end = start + len;
        for (int i = 0; i < (int)clips.size(); ++i) {
            if (i == ignore) continue;
            const auto& c = clips[i];
            if (start < c.endFrame() && c.startFrame < end) return true;
        }
        return false;
    }
};

struct Project {
    int sampleRate = 48000;             // canonical rate (set from audio device mix format)
    std::vector<Clip> library;
    std::vector<Track> tracks;
    int nextClipId = 1;
    int nextTrackId = 1;

    Clip* findClip(int id) {
        for (auto& c : library) if (c.id == id) return &c;
        return nullptr;
    }
    const Clip* findClip(int id) const {
        for (auto& c : library) if (c.id == id) return &c;
        return nullptr;
    }
    Track* findTrack(int id) {
        for (auto& t : tracks) if (t.id == id) return &t;
        return nullptr;
    }
    const Track* findTrack(int id) const {
        for (auto& t : tracks) if (t.id == id) return &t;
        return nullptr;
    }
    int64_t timelineLengthFrames() const {
        int64_t mx = 0;
        for (auto& t : tracks)
            for (auto& pc : t.clips)
                mx = std::max(mx, pc.endFrame());
        return mx;
    }
};
