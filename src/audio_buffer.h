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

// Peak (volume) cache: one value per bucket = max abs sample across channels.
// Used to draw the "volume graph" waveform cheaply at any zoom level.
struct PeakCache {
    static constexpr int kBucketFrames = 256;
    std::vector<float> peaks;   // max |sample| per bucket
    int bucketFrames = kBucketFrames;

    void build(const AudioBuffer& buf) {
        peaks.clear();
        bucketFrames = kBucketFrames;
        const int ch = buf.channels;
        const int64_t nf = buf.frames();
        if (nf <= 0 || ch <= 0) return;
        const int64_t nBuckets = (nf + bucketFrames - 1) / bucketFrames;
        peaks.reserve((size_t)nBuckets);
        const float* s = buf.samples.data();
        for (int64_t b = 0; b < nBuckets; ++b) {
            int64_t f0 = b * bucketFrames;
            int64_t f1 = std::min<int64_t>(f0 + bucketFrames, nf);
            float mx = 0.0f;
            for (int64_t f = f0; f < f1; ++f) {
                const float* fr = s + f * ch;
                for (int c = 0; c < ch; ++c) {
                    float a = std::fabs(fr[c]);
                    if (a > mx) mx = a;
                }
            }
            peaks.push_back(mx);
        }
    }

    // Aggregate peak over a frame range [f0,f1) using the bucket cache.
    float peakInRange(int64_t f0, int64_t f1) const {
        if (peaks.empty() || f1 <= f0) return 0.0f;
        int64_t b0 = f0 / bucketFrames;
        int64_t b1 = (f1 - 1) / bucketFrames;
        b0 = std::max<int64_t>(0, b0);
        b1 = std::min<int64_t>((int64_t)peaks.size() - 1, b1);
        float mx = 0.0f;
        for (int64_t b = b0; b <= b1; ++b) mx = std::max(mx, peaks[b]);
        return mx;
    }
};
using PeakCachePtr = std::shared_ptr<PeakCache>;
