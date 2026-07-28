#pragma once
#include "audio_buffer.h"
#include <string>
#include <vector>
#include <algorithm>

// A named piece of audio in the clip library. Each clip owns its own AudioBuffer
// (shared_ptr, immutable once created). Derived clips (from a selection/crop) get
// their own trimmed copy so they're fully independent.
struct Clip {
    int id = 0;
    std::wstring name;
    std::wstring sourcePath;        // original file on disk, if any
    AudioBufferPtr buffer;
    PeakCachePtr peaks;
    float gain = 1.0f;             // linear volume multiplier
    uint64_t timestamp = 0;        // FILETIME ticks: source file's mtime for loaded
                                   // clips, else the wall-clock time it was created.
                                   // Used for "sort by time" in the library.

    int64_t frames() const { return buffer ? buffer->frames() : 0; }
    int sampleRate() const { return buffer ? buffer->sampleRate : 48000; }
    double durationSec() const { return buffer ? buffer->durationSec() : 0.0; }
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
