#pragma once
#include "audio_buffer.h"
#include <memory>
#include <functional>
#include <cstdint>
#include <vector>
#include <string>

// A source of stereo audio for the engine. render() must be realtime-safe
// (no allocation, no locks) - the engine calls it from the audio thread while
// holding its own lock.
class IPlaybackSource {
public:
    virtual ~IPlaybackSource() = default;
    // Produce up to `frames` STEREO frames into out (size frames*2). Returns the
    // number actually produced; fewer than requested means the source ended.
    virtual int render(float* outStereo, int frames) = 0;
    virtual void seek(int64_t frame) = 0;
    virtual int64_t position() const = 0;
    virtual int64_t total() const = 0;
};

// Plays a sub-range [begin,end) of a stereo AudioBuffer at engine rate.
class BufferSource : public IPlaybackSource {
public:
    BufferSource(AudioBufferPtr buf, int64_t begin, int64_t end, float gain = 1.0f);
    int render(float* outStereo, int frames) override;
    void seek(int64_t frame) override;      // frame relative to begin
    int64_t position() const override { return pos_ - begin_; }
    int64_t total() const override { return end_ - begin_; }
private:
    AudioBufferPtr buf_;
    int64_t begin_, end_, pos_;
    float gain_;
};

// One placed clip on the timeline, captured for realtime mixing.
struct TimelineSegment {
    AudioBufferPtr buf;
    int64_t timelineStart = 0;      // engine frames
    int64_t length = 0;
    float gain = 1.0f;
};

// Mixes all placed clips across all (unmuted) tracks from a global playhead.
class TimelineSource : public IPlaybackSource {
public:
    TimelineSource(std::vector<TimelineSegment> segs, int64_t totalFrames);
    int render(float* outStereo, int frames) override;
    void seek(int64_t frame) override { pos_ = frame; }
    int64_t position() const override { return pos_; }
    int64_t total() const override { return total_; }
private:
    std::vector<TimelineSegment> segs_;
    int64_t pos_ = 0;
    int64_t total_ = 0;
};

// Maps "device frames the sound card has played" to "source frames the user is
// hearing". A source's own position() is the *render* position -- how much has
// been pushed into the output buffer -- which leads what you hear by the whole
// buffer depth and jumps by a chunk on every audio-thread wakeup, so a playhead
// driven from it both stutters and runs early. The engine records one entry per
// buffer fill and queries it with the audio clock's reading.
//
// Pure integer arithmetic, kept out of engine.cpp so the selftest can exercise
// it without a sound card -- see the "playback position" block in selftest.cpp.
struct RenderTimeline {
    struct Write { int64_t devEnd = 0, srcEnd = 0; int32_t devLen = 0, srcLen = 0; };
    static constexpr int kRecs = 64;      // ~2 s of history at a 30 ms buffer
    Write recs[kRecs]{};
    int recCount = 0, recHead = 0;
    int64_t devWritten = 0;               // device frames given to the device since the stream started
    int64_t srcAtReset = 0;               // source position when the history was last cleared

    // Playing, seeking or stopping makes every queued frame describe audio the
    // caller no longer cares about, so the history is dropped. That is also what
    // makes a seek feel instant: the position reads as the new one at once,
    // instead of tracking the stale audio still draining out of the buffer.
    // `devWritten` deliberately keeps counting -- it is tied to the device clock,
    // which never restarts.
    void reset(int64_t srcPos) { recCount = 0; recHead = 0; srcAtReset = srcPos; }

    // `srcLen` is recorded rather than derived from `devLen` so that resampling
    // and a source draining part-way through a chunk are both handled exactly.
    void push(int32_t devLen, int32_t srcLen, int64_t srcEnd) {
        devWritten += devLen;
        recs[recHead] = { devWritten, srcEnd, devLen, srcLen };
        recHead = (recHead + 1) % kRecs;
        if (recCount < kRecs) ++recCount;
    }

    int64_t sourceAt(int64_t devPlayed) const {
        if (recCount == 0) return srcAtReset;
        const Write& newest = recs[(recHead - 1 + kRecs) % kRecs];
        if (devPlayed >= newest.devEnd) return newest.srcEnd;   // all written audio has played
        for (int i = 0; i < recCount; ++i) {
            const Write& r = recs[(recHead - 1 - i + kRecs) % kRecs];
            if (devPlayed >= r.devEnd - r.devLen) {
                // Interpolating *inside* the chunk is what makes the result
                // smooth: chunks are 10-30 ms but a query lands anywhere in one.
                const double back = r.devLen > 0
                    ? (double)(r.devEnd - devPlayed) / (double)r.devLen : 0.0;
                return r.srcEnd - (int64_t)(back * r.srcLen + 0.5);
            }
        }
        return srcAtReset;   // older than anything still remembered
    }
};

class PlaybackEngine {
public:
    PlaybackEngine();
    ~PlaybackEngine();
    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;
    bool init(std::wstring* err = nullptr);
    void shutdown();

    int sampleRate() const { return sampleRate_; }   // audio device (output) rate

    // Rate of the audio in the sources (the project/internal rate). If it differs
    // from the device rate, the engine linear-resamples on the audio thread so the
    // internal project rate can stay >= 48 kHz regardless of the device mix rate.
    void setSourceRate(int rate);

    // Called (from the audio thread) when the active source drains fully.
    void setEndCallback(std::function<void()> cb) { endCb_ = std::move(cb); }

    void play(std::shared_ptr<IPlaybackSource> src, int64_t startFrame = 0);
    void pause();
    void resume();
    void stop();                    // clears source
    void seek(int64_t frame);

    bool isPlaying() const;         // has source AND not paused AND not drained
    bool isPaused() const;
    bool hasSource() const;
    int64_t position() const;
    int64_t total() const;

    // Why the last position() returned what it did: the raw device position, the
    // two candidate answers, and which clamp (if either) bound. The playhead maps
    // three independent clocks onto each other, so when it misbehaves the reported
    // frame number alone cannot say which one was responsible.
    struct PosDiag {
        int64_t devPlayed = 0;   // device frames played (clock + extrapolation)
        int64_t heard = 0;       // what the timeline mapped that to
        int64_t rendered = 0;    // source frames pulled so far (a staircase)
        int64_t devEnd = 0;      // device frames written as of the newest record
        int     recCount = 0;
        bool    drained = false; // devPlayed ran past everything written
        bool    clamped = false; // heard > rendered, so `rendered` won
    };
    PosDiag posDiag() const;

private:
    void threadMain();
    void writeFrames(void* dst, const float* stereo, int frames);

    struct Impl;
    std::unique_ptr<Impl> d_;
    std::function<void()> endCb_;
    int sampleRate_ = 48000;    // device output rate
    int srcRate_ = 0;           // project/source rate (0 = same as device)
};
