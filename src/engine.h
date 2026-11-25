#pragma once
#include "audio_buffer.h"
#include <atomic>
#include <memory>
#include <mutex>
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

// A volume that may be changed while it plays: set from the UI thread, read by
// render() on the audio thread. A change is ramped over kRampFrames frames rather
// than applied as a step, which on a slider being dragged would be heard as a run of
// clicks. The ramp is counted in frames, not in calls: when the engine resamples it
// asks a source for one frame at a time, and a ramp per call would be a step again.
class LiveGain {
public:
    static constexpr int kRampFrames = 512;   // ~11 ms at 48 kHz
    explicit LiveGain(float g = 1.0f) : target_(g), applied_(g), rampTo_(g) {}
    void set(float g) { target_.store(g, std::memory_order_relaxed); }

    // The rest is the audio thread's. A render call takes up any change with
    // begin(), then takes each frame's gain from next() -- or, while steady(), simply
    // value() -- or, for a source silent in this call, jumps there with snap(): with
    // nothing sounding, a step can't click.
    void begin() {
        const float to = target_.load(std::memory_order_relaxed);
        if (to == rampTo_) return;
        rampTo_ = to;
        left_ = kRampFrames;
        step_ = (to - applied_) / (float)kRampFrames;
    }
    bool steady() const { return left_ == 0; }
    float value() const { return applied_; }
    float next() {
        if (left_ > 0) {
            if (--left_ == 0) applied_ = rampTo_;   // land exactly, whatever the rounding
            else applied_ += step_;
        }
        return applied_;
    }
    void snap() { applied_ = rampTo_; left_ = 0; }
private:
    std::atomic<float> target_;
    // Audio thread only: the gain the last frame had, the gain it is heading for, and
    // how far each frame moves it for how many frames more.
    float applied_, rampTo_, step_ = 0.0f;
    int left_ = 0;
};

// Plays a sub-range [begin,end) of a stereo AudioBuffer at engine rate.
class BufferSource : public IPlaybackSource {
public:
    BufferSource(AudioBufferPtr buf, int64_t begin, int64_t end, float gain = 1.0f);
    int render(float* outStereo, int frames) override;
    void seek(int64_t frame) override;      // frame relative to begin
    int64_t position() const override { return pos_ - begin_; }
    int64_t total() const override { return end_ - begin_; }
    // Change the volume while it plays (any thread).
    void setGain(float g) { gain_.set(g); }
private:
    AudioBufferPtr buf_;
    int64_t begin_, end_, pos_;
    LiveGain gain_;
};

// One placed clip on the timeline, captured for realtime mixing.
struct TimelineSegment {
    AudioBufferPtr buf;
    int64_t timelineStart = 0;      // engine frames
    int64_t length = 0;
    float gain = 1.0f;              // clip x track x mix volume, as armed
};

// Mixes all placed clips across all (unmuted) tracks from a global playhead.
class TimelineSource : public IPlaybackSource {
public:
    TimelineSource(std::vector<TimelineSegment> segs, int64_t totalFrames);
    int render(float* outStereo, int frames) override;
    void seek(int64_t frame) override { pos_ = frame; }
    int64_t position() const override { return pos_; }
    int64_t total() const override { return total_; }
    // Change segment `i`'s volume while it plays (any thread): the arrangement is
    // fixed when the source is built, but its volumes are not. Out of range is ignored.
    size_t segmentCount() const { return segs_.size(); }
    void setSegmentGain(size_t i, float g) { if (i < segs_.size()) gains_[i].set(g); }
private:
    std::vector<TimelineSegment> segs_;
    std::unique_ptr<LiveGain[]> gains_;   // parallel to segs_; atomics, so not a vector
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
    // Playing on to another device, whose clock starts from 0 again.
    void restartDevice(int64_t srcPos) { reset(srcPos); devWritten = 0; }

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
    // Start the engine's audio thread, which opens the default output device and
    // plays silence until told otherwise. Every WASAPI object lives on that thread, in
    // COM's multithreaded apartment: the calling (UI) thread is a single-threaded one,
    // and objects created there could not be used from the audio thread without
    // marshalling. Waits until the thread has tried the device. With no output device
    // it still runs, and starts playing as soon as one appears (hasDevice). False,
    // with the reason in `err`, only when it can't run at all (COM, device
    // enumeration); then, and before init or after shutdown, every other call is a
    // quiet no-op (nothing plays, nothing is playing, position 0).
    bool init(std::wstring* err = nullptr);
    void shutdown();

    // The output device's rate. It can change while playing, if the device goes away
    // or the default output changes and the engine moves to another (reopenDevice).
    int sampleRate() const { return sampleRate_.load(); }

    // Rate of the audio in the sources (the project/internal rate). If it differs
    // from the device rate, the engine linear-resamples on the audio thread so the
    // internal project rate can stay >= 48 kHz regardless of the device mix rate.
    void setSourceRate(int rate);

    // Move to the default output device, keeping what is playing: it carries on from
    // where it was, and the playhead holds still meanwhile. The engine does this by
    // itself when the device it plays to goes away (headphones unplugged) or the
    // default output changes (plugged in, chosen in Settings); returns at once, the
    // audio thread does the work.
    void reopenDevice();

    // Where the sound goes now, as one consistent snapshot.
    struct DeviceStatus {
        bool open = false;       // playing to a device...
        std::wstring name;       // ...this one ("Speakers (Realtek Audio)"; empty if Windows gives no name)
        bool none = false;       // not open because there is no output device at all
        std::wstring problem;    // not open for another reason: what went wrong (another
                                 // program holding it exclusively, say); retried every second
    };
    DeviceStatus deviceStatus() const;
    // deviceStatus().open, without copying strings: the window asks on every tick.
    bool hasDevice() const;

    // What Windows' notification of a new default output does: the engine moves there,
    // if it is another device than the one in use. Returns at once.
    void noteDefaultChanged();

    // Called (from the audio thread) when the active source drains fully.
    void setEndCallback(std::function<void()> cb);
    // Called (from the audio thread) when the device the sound goes to changes: another
    // one, none (the last went away), or one again -- not when the same device is
    // reopened. Ask deviceStatus() where it goes now.
    void setDeviceCallback(std::function<void()> cb);

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
    // The audio thread: openEnumerator and openStream, then (once init has been told
    // how that went) renderLoop until shutdown, then closeStream and closeEnumerator --
    // all on that thread.
    void threadMain();
    // The audio thread's halves of opening and closing: the device enumerator (and the
    // notification of default-device changes) once for the thread's life, the stream on
    // the default device each time it is (re)opened. Each open returns an empty string
    // if it worked, else why not.
    std::wstring openEnumerator();
    std::wstring openStream(bool& none);   // `none`: it failed because there is no device at all
    void closeStream();
    void noteOpen(const std::wstring& why, bool none);
    std::wstring currentDeviceId();
    void fire(std::function<void()> PlaybackEngine::* which);
    void closeEnumerator();
    void reopenStream();
    void keepHeardPosition();
    bool onDefaultDevice();
    void renderLoop();
    void writeFrames(void* dst, const float* stereo, int frames);

    struct Impl;
    std::unique_ptr<Impl> d_;
    // The callbacks, set from the UI thread and called from the audio thread.
    std::mutex cbMtx_;
    std::function<void()> endCb_, deviceCb_;
    std::atomic<int> sampleRate_{ 48000 };   // device output rate; set under the engine's lock
    int srcRate_ = 0;           // project/source rate (0 = same as device); under the lock once open
};
