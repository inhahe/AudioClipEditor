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

private:
    void threadMain();
    void writeFrames(void* dst, const float* stereo, int frames);

    struct Impl;
    std::unique_ptr<Impl> d_;
    std::function<void()> endCb_;
    int sampleRate_ = 48000;    // device output rate
    int srcRate_ = 0;           // project/source rate (0 = same as device)
};
