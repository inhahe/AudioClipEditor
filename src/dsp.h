#pragma once
#include "audio_buffer.h"
#include <cstdint>
#include <vector>

namespace dsp {

// --- Loudness (speech-aware) ---
// RMS level of the "active" (non-silent) portion of a buffer, so pauses between
// speech don't drag the measured level down. Returns linear RMS (0..~1).
double speechLoudness(const AudioBuffer& buf);

// --- Voice cleaner (noise reduction) ---
enum class NRAlgorithm { SpectralSubtraction, Wiener, Profile };
enum class NRStrength { Light, Medium, Aggressive };

// --- Audacity-style profile-based noise reduction (spectral noise gating) ---
// Faithful reimplementation of Audacity's "Noise Reduction" effect in its
// released configuration: 2048-sample Hann/Hann STFT at 4 steps per window,
// "second greatest" noise classification over 5 consecutive windows,
// exponential attack (0.02 s) / release (0.10 s) gain smoothing, and
// geometric-mean frequency smoothing of the gain curve. Two-step workflow:
// capture a NoiseProfile from noise-only audio, then reduce.

struct NoiseProfile {
    int sampleRate = 0;          // audio to be cleaned must match this rate
    int windows = 0;             // complete STFT windows accumulated
    double seconds = 0.0;        // length of the profiled audio (UI display)
    std::vector<float> means;    // per-bin mean noise power (all channels pooled)
    bool valid() const { return windows > 0 && !means.empty(); }
};

// Same options (names, ranges, defaults) as Audacity 3.x's dialog.
struct NRProfileOptions {
    float reductionDb = 6.0f;      // "Noise reduction (dB)"           0 .. 48
    float sensitivity = 6.0f;      // "Sensitivity"                 0.01 .. 24
    int   freqSmoothingBands = 6;  // "Frequency smoothing (bands)"    0 .. 12
    bool  residue = false;         // "Noise:" Reduce (false) / Residue (true)
};

struct NROptions {
    NRAlgorithm algorithm = NRAlgorithm::SpectralSubtraction;
    NRStrength strength = NRStrength::Medium;      // SpectralSubtraction / Wiener
    NRProfileOptions profile;                      // Profile algorithm settings
    const NoiseProfile* noiseProfile = nullptr;    // required when algorithm == Profile
};

// "Get Noise Profile": accumulate per-frequency noise statistics from a
// noise-only buffer (every complete window of every channel contributes, as in
// Audacity). Returns an invalid profile (windows == 0) when the buffer is
// shorter than one STFT window (2048 frames).
NoiseProfile computeNoiseProfile(const AudioBuffer& buf);

// Apply profile-based noise reduction. Returns nullptr if the profile is
// invalid or its sample rate does not match the buffer (mirrors Audacity's
// "sample rate of the noise profile must match" rule).
AudioBufferPtr denoiseWithProfile(const AudioBuffer& buf, const NoiseProfile& profile,
                                  const NRProfileOptions& opts);

// Returns a new buffer with background noise reduced. For the auto algorithms
// (SpectralSubtraction / Wiener) the noise profile is estimated from the
// quietest frames (assumes speech has pauses). For NRAlgorithm::Profile the
// captured opts.noiseProfile is used (nullptr / mismatched rate -> nullptr).
AudioBufferPtr denoise(const AudioBuffer& buf, const NROptions& opts);

} // namespace dsp
