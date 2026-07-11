#pragma once
#include <vector>
#include <memory>
#include <cstdint>
#include <algorithm>
#include <cmath>

// Canonical internal audio format: 32-bit float, interleaved, engine sample rate.
// Channels are always 2 (stereo) internally; mono sources are duplicated on decode.
// Downmix to mono / other channel counts happens only at export time.
struct AudioBuffer {
    int sampleRate = 48000;
    int channels = 2;               // internal always 2
    std::vector<float> samples;     // interleaved: frames * channels

    int64_t frames() const {
        return channels ? (int64_t)samples.size() / channels : 0;
    }
    double durationSec() const {
        return sampleRate ? (double)frames() / sampleRate : 0.0;
    }
};
using AudioBufferPtr = std::shared_ptr<AudioBuffer>;

// Min/max (oscilloscope) cache: for each bucket, the minimum and maximum of the
// mono-mixed signal. Filling between lo and hi renders a true waveform envelope
// when zoomed out; zoomed in it becomes an oscilloscope trace. Used to draw the
// waveform cheaply at any zoom without rescanning the whole buffer each paint.
struct PeakCache {
    static constexpr int kBucketFrames = 256;
    std::vector<float> lo;   // min mono sample per bucket (signed)
    std::vector<float> hi;   // max mono sample per bucket (signed)
    int bucketFrames = kBucketFrames;

    bool empty() const { return hi.empty(); }

    void build(const AudioBuffer& buf) {
        lo.clear(); hi.clear();
        bucketFrames = kBucketFrames;
        const int ch = buf.channels;
        const int64_t nf = buf.frames();
        if (nf <= 0 || ch <= 0) return;
        const int64_t nBuckets = (nf + bucketFrames - 1) / bucketFrames;
        lo.reserve((size_t)nBuckets);
        hi.reserve((size_t)nBuckets);
        const float* s = buf.samples.data();
        const float inv = 1.0f / (float)ch;
        for (int64_t b = 0; b < nBuckets; ++b) {
            int64_t f0 = b * bucketFrames;
            int64_t f1 = std::min<int64_t>(f0 + bucketFrames, nf);
            float mn = 1e30f, mx = -1e30f;
            for (int64_t f = f0; f < f1; ++f) {
                const float* fr = s + f * ch;
                float m = 0.0f;
                for (int c = 0; c < ch; ++c) m += fr[c];
                m *= inv;
                if (m < mn) mn = m;
                if (m > mx) mx = m;
            }
            if (mn > mx) { mn = mx = 0.0f; }
            lo.push_back(mn); hi.push_back(mx);
        }
    }

    // Min/max of the mono signal over [f0,f1) from the bucket cache.
    void rangeMinMax(int64_t f0, int64_t f1, float& outLo, float& outHi) const {
        outLo = 0.0f; outHi = 0.0f;
        if (hi.empty() || f1 <= f0) return;
        int64_t b0 = f0 / bucketFrames;
        int64_t b1 = (f1 - 1) / bucketFrames;
        b0 = std::max<int64_t>(0, b0);
        b1 = std::min<int64_t>((int64_t)hi.size() - 1, b1);
        float mn = 1e30f, mx = -1e30f;
        for (int64_t b = b0; b <= b1; ++b) { mn = std::min(mn, lo[b]); mx = std::max(mx, hi[b]); }
        if (mn > mx) { mn = mx = 0.0f; }
        outLo = mn; outHi = mx;
    }

    // Aggregate absolute peak over [f0,f1) (kept for loudness/preview helpers).
    float peakInRange(int64_t f0, int64_t f1) const {
        float mn, mx; rangeMinMax(f0, f1, mn, mx);
        return std::max(std::fabs(mn), std::fabs(mx));
    }
};
using PeakCachePtr = std::shared_ptr<PeakCache>;
